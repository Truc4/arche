#include "ast.h"
#include <stdlib.h>

Decl *new_pool_decl(PoolDecl *p) {
	Decl *d = malloc(sizeof(Decl));
	d->kind = DECL_POOL;
	d->pool = p;
	return d;
}
