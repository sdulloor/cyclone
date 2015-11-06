// AVLTree (server side)
#include<libcyclone.hpp>
#include "../core/dispatcher_layout.hpp"
#include<string.h>
#include "../core/clock.hpp"
#include<boost/log/trivial.hpp>

#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

typedef unsigned long setkey_t;
typedef void         *setval_t;
typedef int bool_t;
#define TRUE 1
#define FALSE 0

#define MAX(a, b) (((a) > (b)) ? (a) : (b))


/*************************************
 * INTERNAL DEFINITIONS
 */

/* Fine for 2^NUM_LEVELS nodes. */
#define NUM_LEVELS 20


/* Internal key values with special meanings. */
#define INVALID_FIELD   (0)    /* Uninitialised field value.     */
#define SENTINEL_KEYMIN ( 1UL) /* Key value of first dummy node. */
#define SENTINEL_KEYMAX (~0UL) /* Key value of last dummy node.  */


/*
 * Used internally be set access functions, so that callers can use
 * key values 0 and 1, without knowing these have special meanings.
 */
#define CALLER_TO_INTERNAL_KEY(_k) ((_k) + 2)


typedef struct node_st
{
  setkey_t k;
  setval_t v;
  int balance; /* right minus left */
  struct node_st *l, *r, *p;
}node_t;

typedef void * set_t;

struct shared_t {
  node_t *sentinel;
  pthread_rwlock_t lock;
};

static struct shared_t shared;
#ifdef INTEL_STM
[[transaction_safe]]
#endif
static void left_rotate(node_t *x)
{
    node_t *xr, *xrl, *p;

    xr = x->r;
    xrl = xr->l;
    p = x->p;

    if(x == p->l)
        p->l = xr;
    else
        p->r = xr;

    x->p = xr;
    x->r = xrl;
    
    xr->p = p;
    xr->l = x;
    if(xrl != NULL)
      xrl->p = x;

    /* fixup balances */
    x->balance = x->balance - MAX(0, xr->balance) - 1;
    xr->balance = xr->balance - MAX(0, (-1)*(x->balance)) - 1;
}

#ifdef INTEL_STM
[[transaction_safe]]
#endif
static void right_rotate(node_t *x)
{
    node_t *xl, *xlr, *p;

    xl = x->l;
    p = x->p;
    xlr = xl->r;

    if(x == p->l)
        p->l = xl;
    else
        p->r = xl;

    x->p = xl;
    x->l = xlr;
    
    xl->p = p;
    xl->r = x;
    
    if(xlr != NULL)
      xlr->p = x;
    
    /* fixup balances */
    x->balance = 1 + MAX(0, (-1)*xl->balance) + x->balance;
    xl->balance = MAX(0, x->balance) + 1 + xl->balance;
}

/* rebalance tree after insertion */
#ifdef INTEL_STM
[[transaction_safe]]
#endif
void rebalance_insert(node_t *root, node_t *node)
{
  node_t *child, *parent;
  while(node != NULL) {
    if(node == root)
      break; /* root is always balanced */
    if(node->balance  == 2) {
      child = node->r;
      if(child->balance < 0) /* left heavy double rotate */
	right_rotate(child);
      left_rotate(node);
      break;
    }
    else if(node->balance == -2) {
      child = node->l;
      if(child->balance > 0) /* right heavy double rotate */
	left_rotate(child);
      right_rotate(node);
      break;
    }
    if(node->balance == 0) {
      break;
    }
    /* tree has grown */
    parent = node->p;
    if(node == parent->l)
      parent->balance -= 1;
    else
      parent->balance += 1;
    node = parent;
  }
}

#ifdef INTEL_STM
[[transaction_safe]]
#endif
/* rebalance tree after deletion */
void rebalance_delete(node_t *root, node_t *node)
{
  node_t *child, *parent;
  while(node != NULL) {
    if(node == root)
      break; /* root is always balanced */
    if(node->balance  == 2) {
      child = node->r;
      if(child->balance < 0) /* left heavy double rotate */
	right_rotate(child);
      left_rotate(node);
      node = node->p;
    }
    else if(node->balance == -2) {
      child = node->l;
      if(child->balance > 0) /* right heavy double rotate */
	left_rotate(child);
      right_rotate(node);
      node = node->p;
    }
    if(node->balance != 0) {
      break; /* tree balanced */
    }
    /* tree has shrunk */
    parent = node->p;
    if(node == parent->l)
      parent->balance += 1;
    else
      parent->balance -= 1;
    node = parent;
    continue;
  }
}


set_t *set_alloc(void)
{
    node_t *set;
    set = (node_t *)malloc(sizeof(node_t));
    set->k = SENTINEL_KEYMIN;
    set->l = set->r = set->p = NULL;
    set->balance = 0;
    pthread_rwlock_init(&shared.lock, NULL);
    return (set_t *)set;
}


setval_t set_update(set_t *s, setkey_t k, setval_t v, int overwrite)
{
  node_t  *_new, *y, *x, *root;
  setval_t ov;
  int delta;
  k = CALLER_TO_INTERNAL_KEY(k);
  
  _new = (node_t *)malloc(sizeof(node_t));
#ifdef INTEL_STM
  __transaction {
#else
  pthread_rwlock_wrlock(&shared.lock);
#endif
  x = y = root = (node_t *)s;
  while (y  != NULL)
    {
      if ( k == y->k ) break;
      x = y;
      y = (k < y->k) ? y->l : y->r;
    }
  if ( y != NULL )
    {
      ov = y->v;
      if ( overwrite ) {
	y->v = v;
      }
	assert(ov != NULL);
    }
  else
    {
      ov = NULL;
      _new->k = k;
      _new->v = v;
      _new->l = NULL;
      _new->r = NULL;
      _new->p = x; 
      _new->balance = 0;
      if ( k < x->k ) {
	x->l = _new; 
	x->balance -= 1;
      }
      else {
	x->r = _new;
	x->balance += 1;
      }
      rebalance_insert(root, x);
    }
  //check_tree(root->r, root->v);
#ifdef INTEL_STM
  }
#else
  pthread_rwlock_unlock(&shared.lock);
#endif
  
  if ( (ov != NULL) ) {
    free(_new);
  }
  return ov;
}


setval_t set_remove(set_t *s, setkey_t k)
{
  node_t  *y, *removed, *p, *root;
  setval_t ov;
  int delta;
  k = CALLER_TO_INTERNAL_KEY(k);
#ifdef INTEL_STM
  __transaction {
#else
  pthread_rwlock_wrlock(&shared.lock);
#endif
  ov = NULL;
  root = y = (node_t *)s;
  while ( y != NULL )
    {
      if ( k == y->k )
	{
	  ov = y->v;
	  break;
	}
      y = (k < y->k) ? y->l : y->r;
    }
    
  if ( y != NULL )
    {
      if ( (y->l != NULL) && (y->r != NULL) )
	{
	  /* Switch with an appropriate successor */
	  removed = y->r;
	  
	  while(removed->l != NULL)
	    removed = removed->l;
	  y->k = removed->k;
	  y->v = removed->v;
	}
      else
	{
	  removed = y;
	}
      
      /* Now we need to delete removed at least one of whose children is NULL */
      if(removed->r != NULL)
	y = removed->r;
      else 
	y= removed->l;
      p = removed->p;
      if(y)
	y->p = p;
      if(p->l == removed) {
	p->l = y;
	p->balance += 1;
      }
      else if(p->r == removed){
	p->r = y;
	p->balance -= 1;
      }
#ifndef INTEL_STM
      else {
	printf("node in tree and not in it !!!\n");
	exit(-1);
      }
#endif
      rebalance_delete(root, p);
    }
  //check_tree(root->r, root->v);
#ifdef INTEL_STM
  }
#else
  pthread_rwlock_unlock(&shared.lock);
#endif
  
  if ( ov != NULL ) {
    free(removed);
  }
  
  return ov;
}

setval_t set_lookup(set_t *s, setkey_t k)
{

    node_t  *n;
    setval_t v;
    k = CALLER_TO_INTERNAL_KEY(k);
#ifdef INTEL_STM
    __transaction  {
#else
    pthread_rwlock_rdlock(&shared.lock);
#endif
    v  = NULL;
    n = (node_t *)s;
    while ( n != NULL)
        {
            if ( k == n->k )
            {
	      v = n->v;
	      break;
            }
            n = (k < n->k) ? n->l : n->r;

        }
#ifdef INTEL_STM
    }
#else
    pthread_rwlock_unlock(&shared.lock);
#endif
    return v;
}

static void **set;

TOID(char) nvheap_setup(TOID(char) recovered)
{
  return TOID_NULL(char);
}

void gc (void *data)
{
  free(data);
}

int callback(const unsigned char *data,
	     const int len,
	     void **return_value)
{
  int code = *(int *)data;
  if(code == 0) { // lookup
    setval_t v = set_lookup(set, *(setkey_t *)(data + sizeof(int)));
    void *ret_val = malloc(sizeof(setval_t));
    memcpy(ret_val, &v, sizeof(setval_t));
    *return_value = ret_val;
    return sizeof(setval_t);
  }
  else if(code == 1) { // Insert
    setval_t v = set_update(set,
			    *(setkey_t *)(data + sizeof(int)),
			    *(setval_t *)(data + sizeof(int) + sizeof(setkey_t)),
			    0);
    void *ret_val = malloc(sizeof(setval_t));
    memcpy(ret_val, &v, sizeof(setval_t));
    *return_value = ret_val;
    return sizeof(setval_t);
  }
  else if(code == 2) { // Update (insert or overwrite)
    setval_t v = set_update(set,
			    *(setkey_t *)(data + sizeof(int)),
			    *(setval_t *)(data + sizeof(int) + sizeof(setkey_t)),
			    1);
    void *ret_val = malloc(sizeof(setval_t));
    memcpy(ret_val, &v, sizeof(setval_t));
    *return_value = ret_val;
    return sizeof(setval_t);
  }
  else if(code == 3) { // Delete
    setval_t v = set_remove(set, *(setkey_t *)(data + sizeof(int)));
    void *ret_val = malloc(sizeof(setval_t));
    memcpy(ret_val, &v, sizeof(setval_t));
    *return_value = ret_val;
    return sizeof(setval_t);
  }
  else {
    BOOST_LOG_TRIVIAL(fatal) << "Unknown set opcode";
    exit(-1);
  }
}

int main(int argc, char *argv[])
{
  dispatcher_start("cyclone_test.ini", callback, gc, nvheap_setup);
}
