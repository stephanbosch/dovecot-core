#ifndef __AUTH_H
#define __AUTH_H

#include "auth-interface.h"

typedef void (*AuthCallback) (AuthReplyData *reply, const unsigned char *data,
			      void *user_data);

typedef struct {
	AuthMethod method;

	void (*init)(AuthInitRequestData *request,
		     AuthCallback callback, void *user_data);
} AuthModule;

extern AuthMethod auth_methods;
extern char *const *auth_realms;

void auth_register_module(AuthModule *module);
void auth_unregister_module(AuthModule *module);

void auth_init_request(AuthInitRequestData *request,
		       AuthCallback callback, void *user_data);
void auth_continue_request(AuthContinuedRequestData *request,
			   const unsigned char *data,
			   AuthCallback callback, void *user_data);

void auth_init(void);
void auth_deinit(void);

#endif
