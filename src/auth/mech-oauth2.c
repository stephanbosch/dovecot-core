/* Copyright (c) 2017-2018 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "auth-fields.h"
#include "auth-worker-connection.h"
#include "str.h"
#include "strescape.h"
#include "json-generator.h"
#include "mech.h"
#include "passdb.h"
#include "db-oauth2.h"
#include "oauth2.h"

struct oauth2_auth_request {
	struct auth_request auth;
	struct db_oauth2 *db;
	struct db_oauth2_request db_req;
	lookup_credentials_callback_t *callback;
	bool failed:1;
};

/* RFC5801 based unescaping */
static bool oauth2_unescape_username(const char *in, const char **username_r)
{
	string_t *out;
	out = t_str_new(64);
	for (; *in != '\0'; in++) {
		if (in[0] == ',')
			return FALSE;
		if (in[0] == '=') {
			if (in[1] == '2' && in[2] == 'C')
				str_append_c(out, ',');
			else if (in[1] == '3' && in[2] == 'D')
				str_append_c(out, '=');
			else
				return FALSE;
			in += 2;
		} else {
			str_append_c(out, *in);
		}
	}
	*username_r = str_c(out);
	return TRUE;
}

static void
oauth2_send_failure(struct oauth2_auth_request *oauth2_req, int code,
		    const char *status)
{
	struct auth_request *request = &oauth2_req->auth;
	const char *oidc_url = db_oauth2_get_openid_configuration_url(oauth2_req->db);
	string_t *str = t_str_new(256);
	struct json_generator *gen = json_generator_init_str(str, 0);
	json_generate_object_open(gen);

	if (strcmp(request->mech->mech_name, "XOAUTH2") == 0) {
		status = dec2str(code);
		json_generate_object_member(gen, "schemes");
		json_generate_string(gen, "bearer");
	}

	json_generate_object_member(gen, "status");
	json_generate_string(gen, status);
	json_generate_object_member(gen, "scope");
	json_generate_string(gen, "mail");
	json_generate_object_member(gen, "openid-configuration");
	json_generate_string(gen, oidc_url);
	json_generate_object_close(gen);
	json_generator_deinit(&gen);

	oauth2_req->failed = TRUE;
	auth_request_fail_with_reply(request, str->data, str->used);
}

static void
oauth2_verify_callback(enum passdb_result result,
		       const unsigned char *credentials ATTR_UNUSED,
		       size_t size ATTR_UNUSED, struct auth_request *request)
{
	struct oauth2_auth_request *oauth2_req =
		container_of(request, struct oauth2_auth_request, auth);

	switch (result) {
	case PASSDB_RESULT_INTERNAL_FAILURE:
		/* Non-standard response */
		oauth2_send_failure(oauth2_req, 500, "internal_failure");
		break;
	case PASSDB_RESULT_USER_DISABLED:
	case PASSDB_RESULT_PASS_EXPIRED:
		/* user is explicitly disabled, don't allow it to log in */
		oauth2_send_failure(oauth2_req, 403, "insufficient_scope");
		return;
	case PASSDB_RESULT_PASSWORD_MISMATCH:
		oauth2_send_failure(oauth2_req, 401, "invalid_token");
		break;
	case PASSDB_RESULT_NEXT:
	case PASSDB_RESULT_SCHEME_NOT_AVAILABLE:
	case PASSDB_RESULT_USER_UNKNOWN:
	case PASSDB_RESULT_OK:
		/* sending success */
		auth_request_success(request, "", 0);
		break;
	default:
		i_unreached();
	}
}

static void
mech_oauth2_verify_token_continue(struct oauth2_auth_request *oauth2_req,
				  const char *const *args)
{
	struct auth_request *request = &oauth2_req->auth;
	int parsed;
	enum passdb_result result;

	/* OK result user fields */
	if (args[0] == NULL || args[1] == NULL) {
		result = PASSDB_RESULT_INTERNAL_FAILURE;
		e_error(request->mech_event, "BUG: Invalid auth worker response: empty");
	} else if (str_to_int(args[1], &parsed) < 0) {
		result = PASSDB_RESULT_INTERNAL_FAILURE;
		e_error(request->mech_event, "BUG: Invalid auth worker response: cannot parse '%s'", args[1]);
	} else if (args[2] == NULL) {
		result = PASSDB_RESULT_INTERNAL_FAILURE;
		e_error(request->mech_event, "BUG: Invalid auth worker response: cannot parse '%s'", args[1]);
	} else {
		result = parsed;
	}

	if (result == PASSDB_RESULT_OK) {
		request->passdb_success = TRUE;
		auth_request_set_password_verified(request);
		auth_request_set_fields(request, args + 3, NULL);
		auth_request_lookup_credentials(request, "", oauth2_verify_callback);
		auth_request_unref(&request);
		return;
	}

	oauth2_verify_callback(result, uchar_empty_ptr, 0, request);
	auth_request_unref(&request);
}

static bool
mech_oauth2_verify_token_input_args(struct auth_worker_connection *conn ATTR_UNUSED,
				     const char *const *args, void *context)
{
	struct oauth2_auth_request *oauth2_req = context;
	mech_oauth2_verify_token_continue(oauth2_req, args);
	return TRUE;
}

static void
mech_oauth2_verify_token_local_continue(struct db_oauth2_request *db_req,
					enum passdb_result result,
					const char *error,
					struct oauth2_auth_request *oauth2_req)
{
	struct auth_request *request = &oauth2_req->auth;
	if (result == PASSDB_RESULT_OK) {
		auth_request_set_password_verified(request);
		auth_request_set_field(request, "token", db_req->token, NULL);
		auth_request_lookup_credentials(request, "", oauth2_verify_callback);
		auth_request_unref(&request);
		pool_unref(&db_req->pool);
		return;
	} else {
		e_error(request->mech_event, "Token verification failed: %s", error);
	}
	oauth2_verify_callback(result, uchar_empty_ptr, 0, request);
	auth_request_unref(&request);
	pool_unref(&db_req->pool);
}

static void
mech_oauth2_verify_token(struct oauth2_auth_request *oauth2_req, const char *token)
{
	struct auth_request *auth_request = &oauth2_req->auth;
	auth_request_ref(auth_request);

	if (!db_oauth2_use_worker(oauth2_req->db)) {
		pool_t pool = pool_alloconly_create(MEMPOOL_GROWING"oauth2 request", 256);
		struct db_oauth2_request *db_req =
			p_new(pool, struct db_oauth2_request, 1);
		db_req->pool = pool;
		db_req->auth_request = auth_request;
		db_oauth2_lookup(oauth2_req->db, db_req, token, db_req->auth_request,
				 mech_oauth2_verify_token_local_continue, oauth2_req);
	} else {
		string_t *str = t_str_new(128);
		str_append(str, "TOKEN\tOAUTH2\t");
		str_append_tabescaped(str, token);
		str_append_c(str, '\t');
		auth_request_export(auth_request, str);
		auth_worker_call(oauth2_req->db_req.pool, auth_request->fields.user,
				 str_c(str), mech_oauth2_verify_token_input_args, oauth2_req);
	}
}

static void oauth2_init_db(struct oauth2_auth_request *oauth2_req)
{
	if (oauth2_req->db != NULL)
		return;
	if (*oauth2_req->auth.set->oauth2_config_file != '\0')
		oauth2_req->db = db_oauth2_init(oauth2_req->auth.set->oauth2_config_file);
	else {
		e_error(oauth2_req->auth.mech_event, "Cannot initialize oauth2");
		oauth2_req->failed = TRUE;
		auth_request_internal_failure(&oauth2_req->auth);
	}
}

/* Input syntax:
 user=Username^Aauth=Bearer token^A^A
*/
static void
mech_xoauth2_auth_continue(struct auth_request *request,
			   const unsigned char *data,
			   size_t data_size)
{
	struct oauth2_auth_request *oauth2_req =
		container_of(request, struct oauth2_auth_request, auth);
	oauth2_init_db(oauth2_req);
	/* Specification says that client is sent "invalid token" challenge
	   which the client is supposed to ack with empty response */
	if (oauth2_req->failed)
		return;

	if (data_size == 0) {
		 oauth2_send_failure(oauth2_req, 401, "invalid_token");
		 return;
	}

	/* split the data from ^A */
	bool user_given = FALSE;
	const char *value;
	const char *error;
	const char *token = NULL;
	const char *const *ptr;
	const char *username;
	const char *const *fields =
		t_strsplit(t_strndup(data, data_size), "\x01");
	for(ptr = fields; *ptr != NULL; ptr++) {
		if (str_begins(*ptr, "user=", &value)) {
			/* xoauth2 does not require unescaping because the data
			   format does not contain anything to escape */
			username = value;
			user_given = TRUE;
		} else if (str_begins(*ptr, "auth=", &value)) {
			if (str_begins_icase(value, "bearer ", &value) &&
			    oauth2_valid_token(value)) {
				token = value;
			} else {
				e_info(request->mech_event,
				       "Invalid continued data");
				oauth2_send_failure(oauth2_req, 401,
						    "invalid_token");
				return;
			}
		}
		/* do not fail on unexpected fields */
	}

	if (user_given && !auth_request_set_username(request, username, &error)) {
		e_info(request->mech_event,
		       "%s", error);
		oauth2_send_failure(oauth2_req, 400, "invalid_request");
		return;
	}
	if (user_given && token != NULL)
		mech_oauth2_verify_token(oauth2_req, token);
	else if (token == NULL) {
		e_info(request->mech_event, "Missing token");
		oauth2_send_failure(oauth2_req, 401, "invalid_token");
	} else {
		e_info(request->mech_event, "Missing username");
		oauth2_send_failure(oauth2_req, 401, "invalid_token");
	}
}

/* Input syntax for data:
 gs2flag,a=username,^Afield=...^Afield=...^Aauth=Bearer token^A^A
*/
static void
mech_oauthbearer_auth_continue(struct auth_request *request,
			       const unsigned char *data,
			       size_t data_size)
{
	struct oauth2_auth_request *oauth2_req =
		container_of(request, struct oauth2_auth_request, auth);
	oauth2_init_db(oauth2_req);
	if (oauth2_req->failed)
		return;
	if (data_size == 0) {
		 oauth2_send_failure(oauth2_req, 401, "invalid_token");
		 return;
	}

	bool user_given = FALSE;
	const char *value, *error;
	const char *username;
	const char *const *ptr;
	/* split the data from ^A */
	const char *const *fields =
		t_strsplit(t_strndup(data, data_size), "\x01");
	const char *token = NULL;
	/* ensure initial field is OK */
	if (*fields == NULL || *(fields[0]) == '\0') {
		e_info(request->mech_event,
		       "Invalid continued data");
		oauth2_send_failure(oauth2_req, 401, "invalid_token");
		return;
	}

	/* the first field is specified by RFC5801 as gs2-header */
	for(ptr = t_strsplit_spaces(fields[0], ","); *ptr != NULL; ptr++) {
		switch(*ptr[0]) {
		case 'f':
			e_info(request->mech_event,
			       "Client requested non-standard mechanism");
			oauth2_send_failure(oauth2_req, 400,
					    "request_not_supported");
			return;
		case 'p':
			/* channel binding is not supported */
			e_info(request->mech_event,
			       "Client requested and used channel-binding");
			oauth2_send_failure(oauth2_req, 400,
					    "request_not_supported");
			return;
		case 'n':
		case 'y':
			/* we don't need to use channel-binding */
			continue;
		case 'a': /* authzid */
			if ((*ptr)[1] != '=' ||
			    !oauth2_unescape_username((*ptr)+2, &username)) {
				 e_info(request->mech_event,
					"Invalid username escaping");
				 oauth2_send_failure(oauth2_req, 400,
						     "invalid_request");
				 return;
			} else {
				user_given = TRUE;
			}
			break;
		default:
			e_info(request->mech_event,
			       "Invalid gs2-header in request");
			oauth2_send_failure(oauth2_req, 400, "invalid_request");
			return;
		}
	}

	for(ptr = fields; *ptr != NULL; ptr++) {
		if (str_begins(*ptr, "auth=", &value)) {
			if (str_begins_icase(value, "bearer ", &value) &&
			    oauth2_valid_token(value)) {
				token = value;
			} else {
				e_info(request->mech_event,
				       "Invalid continued data");
				oauth2_send_failure(oauth2_req, 401,
						    "invalid_token");
				return;
			}
		}
		/* do not fail on unexpected fields */
	}
	if (user_given && !auth_request_set_username(request, username, &error)) {
		e_info(request->mech_event,
		       "%s", error);
		oauth2_send_failure(oauth2_req, 400, "invalid_request");
		return;
	}
	if (user_given && token != NULL)
		mech_oauth2_verify_token(oauth2_req, token);
	else if (token == NULL) {
		e_info(request->mech_event, "Missing token");
		oauth2_send_failure(oauth2_req, 401, "invalid_token");
	} else {
		e_info(request->mech_event, "Missing username");
		oauth2_send_failure(oauth2_req, 401, "invalid_request");
	}
}

static struct auth_request *mech_oauth2_auth_new(void)
{
	struct oauth2_auth_request *request;
	pool_t pool = pool_alloconly_create_clean(MEMPOOL_GROWING
						  "oauth2_auth_request", 2048);
	request = p_new(pool, struct oauth2_auth_request, 1);
	request->auth.pool = pool;
	request->db_req.pool = pool;
	return &request->auth;
}

const struct mech_module mech_oauthbearer = {
	"OAUTHBEARER",

	/* while this does not transfer plaintext password,
	   the token is still considered as password */
	.flags = MECH_SEC_PLAINTEXT,
	.passdb_need = 0,

	mech_oauth2_auth_new,
	mech_generic_auth_initial,
	mech_oauthbearer_auth_continue,
	mech_generic_auth_free
};

const struct mech_module mech_xoauth2 = {
	"XOAUTH2",

	.flags = MECH_SEC_PLAINTEXT,
	.passdb_need = 0,

	mech_oauth2_auth_new,
	mech_generic_auth_initial,
	mech_xoauth2_auth_continue,
	mech_generic_auth_free
};


