/*
 * Itsy Driver Utility Header
 * Copyright (c) Carl Waldspurger, 1999.
 *
 * $Log: itsy_util.h,v $
 * Revision 2.2  1999/02/19 19:38:23  caw
 * Defined KOBJ_DEALLOC() macro.
 *
 * Revision 2.1  1999/02/12  22:41:28  caw
 * Initial revision.
 *
 */

#ifndef	ITSY_UTIL_H
#define	ITSY_UTIL_H

/*
 * useful macros
 *
 */

#ifndef	MIN
#define MIN(a,b)                (((a)<(b))?(a):(b))
#define MAX(a,b)                (((a)>(b))?(a):(b))
#endif

#define	KOBJ_ALLOC(obj)		((obj *) kmalloc(sizeof(obj), GFP_KERNEL))
#define	KOBJ_INIT(x, obj)	(memset((void *) (x), 0, sizeof(obj)))
#define	KOBJ_DEALLOC(x, obj)	(kfree_s((void *) x, sizeof(obj)))

#define	SAFE_STRING(s)		(((s) != NULL) ? (s) : "")

/*
 * generic list operation macros 
 *
 */

#define	INSTANTIATE_LIST_LOOKUP(ELT)			\
static ELT * ELT ## _lookup(ELT *head, int id)		\
{							\
  ELT *e;						\
							\
  /* search element list for matching id */		\
  for (e = head; e != NULL; e = e->next)		\
    if (e->id == id)					\
      return(e);					\
							\
  /* not found */					\
  return(NULL);						\
}

#define	INSTANTIATE_LIST_INSERT(ELT)			\
static void ELT ## _insert(ELT **head, ELT *e)		\
{							\
  ELT *h = *head;					\
							\
  /* add element to head of list */			\
  e->next = h;						\
  if (h != NULL)					\
    h->prev = e;					\
  *head = e;						\
  e->prev = NULL;					\
}

#define	INSTANTIATE_LIST_REMOVE(ELT)			\
static void ELT ## _remove(ELT **head, ELT *e)		\
{							\
  /* splice element out of list */		  	\
  if (e->prev != NULL)					\
    e->prev->next = e->next;				\
  else							\
    *head = e->next;					\
  if (e->next != NULL)					\
    e->next->prev = e->prev;				\
}

#endif	/* ITSY_UTIL_H */
