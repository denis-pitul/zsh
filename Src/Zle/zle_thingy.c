/*
 * zle_thingy.c - thingies
 *
 * This file is part of zsh, the Z shell.
 *
 * Copyright (c) 1992-1997 Paul Falstad
 * All rights reserved.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and to distribute modified versions of this software for any
 * purpose, provided that the above copyright notice and the following
 * two paragraphs appear in all copies of this software.
 *
 * In no event shall Paul Falstad or the Zsh Development Group be liable
 * to any party for direct, indirect, special, incidental, or consequential
 * damages arising out of the use of this software and its documentation,
 * even if Paul Falstad and the Zsh Development Group have been advised of
 * the possibility of such damage.
 *
 * Paul Falstad and the Zsh Development Group specifically disclaim any
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose.  The software
 * provided hereunder is on an "as is" basis, and Paul Falstad and the
 * Zsh Development Group have no obligation to provide maintenance,
 * support, updates, enhancements, or modifications.
 *
 */

#include "zle.mdh"
#include "zle_thingy.pro"

/*
 * Thingies:
 *
 * From the user's point of view, a thingy is just a string.  Internally,
 * the thingy is a struct thingy; these structures are in a hash table
 * indexed by the string the user sees.  This hash table contains all
 * thingies currently referenced anywhere; each has a reference count,
 * and is deleted when it becomes unused.  Being the name of a function
 * counts as a reference.
 *
 * The DISABLED flag on a thingy indicates that it is not the name of a
 * widget.  This makes it easy to generate completion lists;
 * looking only at the `enabled' nodes makes the thingy table look like
 * a table of widgets.
 */

/* Hashtable of thingies.  Enabled nodes are those that refer to widgets. */

/**/
mod_export HashTable thingytab;

/**********************************/
/* hashtable management functions */
/**********************************/

/**/
static void
createthingytab(void)
{
    thingytab = newhashtable(199, "thingytab", NULL);

    thingytab->hash        = hasher;
    thingytab->emptytable  = emptythingytab;
    thingytab->filltable   = NULL;
    thingytab->cmpnodes    = strcmp;
    thingytab->addnode     = addhashnode;
    thingytab->getnode     = gethashnode;
    thingytab->getnode2    = gethashnode2;
    thingytab->removenode  = removehashnode;
    thingytab->disablenode = NULL;
    thingytab->enablenode  = NULL;
    thingytab->freenode    = freethingynode;
    thingytab->printnode   = NULL;
}

/**/
static void
emptythingytab(UNUSED(HashTable ht))
{
    /* This will only be called when deleting the thingy table, which *
     * is only done to unload the zle module.  A normal emptytable()  *
     * function would free all the thingies, but we don't want to do  *
     * that because some of them are the known thingies in the fixed  *
     * `thingies' table.  As the module cleanup code deletes all the  *
     * keymaps and so on before deleting the thingy table, we can     *
     * just remove the user-defined widgets and then be sure that     *
     * *all* the thingies left are the fixed ones.  This has the side *
     * effect of freeing all resources used by user-defined widgets.  */
    scanhashtable(thingytab, 0, 0, DISABLED, scanemptythingies, 0);
}

/**/
static void
scanemptythingies(HashNode hn, UNUSED(int flags))
{
    Thingy t = (Thingy) hn;

    /* Mustn't unbind internal widgets -- we wouldn't want to free the *
     * memory they use.                                                */
    if(!(t->widget->flags & WIDGET_INT))
	unbindwidget(t, 1);
}

/**/
static Thingy
makethingynode(void)
{
    Thingy t = (Thingy) zshcalloc(sizeof(*t));

    t->flags = DISABLED;
    return t;
}

/**/
static void
freethingynode(HashNode hn)
{
    Thingy th = (Thingy) hn;

    zsfree(th->nam);
    zfree(th, sizeof(*th));
}

/************************/
/* referencing thingies */
/************************/

/* It is important to maintain the reference counts on thingies.  When *
 * copying a reference to a thingy, wrap the copy in refthingy(), to   *
 * increase its reference count.  When removing a reference,           *
 * unrefthingy() it.  Both of these functions handle NULL arguments    *
 * correctly.                                                          */

/**/
mod_export Thingy
refthingy(Thingy th)
{
    if(th)
	th->rc++;
    return th;
}

/**/
void
unrefthingy(Thingy th)
{
    if(th && !--th->rc)
	thingytab->freenode(thingytab->removenode(thingytab, th->nam));
}

/* Use rthingy() to turn a string into a thingy.  It increases the reference *
 * count, after creating the thingy structure if necessary.                  */

/**/
Thingy
rthingy(char *nam)
{
    Thingy t = (Thingy) thingytab->getnode2(thingytab, nam);

    if(!t)
	thingytab->addnode(thingytab, ztrdup(nam), t = makethingynode());
    return refthingy(t);
}

/**/
Thingy
rthingy_nocreate(char *nam)
{
    Thingy t = (Thingy) thingytab->getnode2(thingytab, nam);

    if(!t)
	return NULL;
    return refthingy(t);
}

/***********/
/* widgets */
/***********/

/*
 * Each widget is attached to one or more thingies.  Each thingy
 * names either zero or one widgets.  Thingies that name a widget
 * are treated as being referenced.  The widget type, flags and pointer
 * are stored in a separate structure pointed to by the thingies.  Each
 * thingy also has a pointer to the `next' thingy (in a circular list)
 * that references the same widget.  The DISABLED flag is unset in these
 * thingies.
 */

/* Bind a widget to a thingy.  The thingy's reference count must already *
 * have been incremented.  The widget may already be bound to other      *
 * thingies; if it is not, then its `first' member must be NULL.  Return *
 * is 0 on success, or -1 if the thingy has the TH_IMMORTAL flag set.    */

/**/
static int
bindwidget(Widget w, Thingy t)
{
    if(t->flags & TH_IMMORTAL) {
	unrefthingy(t);
	return -1;
    }
    if(!(t->flags & DISABLED)) {
	if(t->widget == w)
	    return 0;
	unbindwidget(t, 1);
    }
    if(w->first) {
	t->samew = w->first->samew;
	w->first->samew = t;
    } else {
	w->first = t;
	t->samew = t;
    }
    t->widget = w;
    t->flags &= ~DISABLED;
    return 0;
}

/* Unbind a widget from a thingy.  This decrements the thingy's reference *
 * count.  The widget will be destroyed if this is its last name.         *
 * TH_IMMORTAL thingies won't be touched, unless override is non-zero.    *
 * Returns 0 on success, or -1 if the thingy is protected.  If the thingy *
 * doesn't actually reference a widget, this is considered successful.    */

/**/
static int
unbindwidget(Thingy t, int override)
{
    Widget w;

    if(t->flags & DISABLED)
	return 0;
    if(!override && (t->flags & TH_IMMORTAL))
	return -1;
    w = t->widget;
    if(t->samew == t)
	freewidget(w);
    else {
	Thingy p;
	for(p = w->first; p->samew != t; p = p->samew) ;
	w->first = p;   /* optimised for deletezlefunction() */
	p->samew = t->samew;
    }
    t->flags &= ~TH_IMMORTAL;
    t->flags |= DISABLED;
    unrefthingy(t);
    return 0;
}

/* Free a widget. */

/**/
static void
freewidget(Widget w)
{
    if (w->flags & WIDGET_NCOMP) {
	zsfree(w->u.comp.wid);
	zsfree(w->u.comp.func);
    } else if(!(w->flags & WIDGET_INT))
	zsfree(w->u.fnnam);
    zfree(w, sizeof(*w));
}

/* Add am internal widget provided by a module.  The name given is the  *
 * canonical one, which must not begin with a dot.  The widget is first *
 * bound to the dotted canonical name; if that name is already taken by *
 * an internal widget, failure is indicated.  The same widget is then   *
 * bound to the canonical name, and a pointer to the widget structure   *
 * returned.                                                            */

/**/
mod_export Widget
addzlefunction(char *name, ZleIntFunc ifunc, int flags)
{
    VARARR(char, dotn, strlen(name) + 2);
    Widget w;
    Thingy t;

    if(name[0] == '.')
	return NULL;
    dotn[0] = '.';
    strcpy(dotn + 1, name);
    t = (Thingy) thingytab->getnode(thingytab, dotn);
    if(t && (t->flags & TH_IMMORTAL))
	return NULL;
    w = zalloc(sizeof(*w));
    w->flags = WIDGET_INT | flags;
    w->first = NULL;
    w->u.fn = ifunc;
    t = rthingy(dotn);
    bindwidget(w, t);
    t->flags |= TH_IMMORTAL;
    bindwidget(w, rthingy(name));
    return w;
}

/* Delete an internal widget provided by a module.  Don't try to delete *
 * a widget from the fixed table -- it would be bad.  (Thanks, Egon.)   */

/**/
mod_export void
deletezlefunction(Widget w)
{
    Thingy p, n;

    p = w->first;
    while(1) {
	n = p->samew;
	if(n == p) {
	    unbindwidget(p, 1);
	    return;
	}
	unbindwidget(p, 1);
	p = n;
    }
}

/***************/
/* zle builtin */
/***************/

/*
 * The available operations are:
 *
 *   -l   list widgets/test for existence
 *   -D   delete widget names
 *   -A   link the two named widgets (2 arguments)
 *   -C   create completion widget (3 arguments)
 *   -N   create new user-defined widget (1 or 2 arguments)
 *        invoke a widget (1 argument)
 */

/**/
int
bin_zle(char *name, char **args, Options ops, UNUSED(int func))
{
    static struct opn {
	char o;
	int (*func) _((char *, char **, Options, char));
	int min, max;
    } const opns[] = {
	{ 'l', bin_zle_list, 0, -1 },
	{ 'D', bin_zle_del,  1, -1 },
	{ 'A', bin_zle_link, 2,  2 },
	{ 'N', bin_zle_new,  1,  2 },
	{ 'C', bin_zle_complete, 3, 3 },
	{ 'R', bin_zle_refresh, 0, -1 },
	{ 'M', bin_zle_mesg, 1, 1 },
	{ 'U', bin_zle_unget, 1, 1 },
	{ 'K', bin_zle_keymap, 1, 1 },
	{ 'I', bin_zle_invalidate, 0, 0 },
	{ 'F', bin_zle_fd, 0, 2 },
	{ 0,   bin_zle_call, 0, -1 },
    };
    struct opn const *op, *opp;
    int n;

    /* select operation and ensure no clashing arguments */
    for(op = opns; op->o && !OPT_ISSET(ops,STOUC(op->o)); op++) ;
    if(op->o)
	for(opp = op; (++opp)->o; )
	    if(OPT_ISSET(ops,STOUC(opp->o))) {
		zwarnnam(name, "incompatible operation selection options",
		    NULL, 0);
		return 1;
	    }

    /* check number of arguments */
    for(n = 0; args[n]; n++) ;
    if(n < op->min) {
	zwarnnam(name, "not enough arguments for -%c", NULL, op->o);
	return 1;
    } else if(op->max != -1 && n > op->max) {
	zwarnnam(name, "too many arguments for -%c", NULL, op->o);
	return 1;
    }

    /* pass on the work to the operation function */
    return op->func(name, args, ops, op->o);
}

/**/
static int
bin_zle_list(UNUSED(char *name), char **args, Options ops, UNUSED(char func))
{
    if (!*args) {
	scanhashtable(thingytab, 1, 0, DISABLED, scanlistwidgets,
		      (OPT_ISSET(ops,'a') ? -1 : OPT_ISSET(ops,'L')));
	return 0;
    } else {
	int ret = 0;
	Thingy t;

	for (; *args && !ret; args++) {
	    if (!(t = (Thingy) thingytab->getnode2(thingytab, *args)) ||
		(!OPT_ISSET(ops,'a') && (t->widget->flags & WIDGET_INT)))
		ret = 1;
	}
	return ret;
    }
}

/**/
static int
bin_zle_refresh(UNUSED(char *name), char **args, Options ops, UNUSED(char func))
{
    ZLE_STRING_T s = statusline;
    int sl = statusll, ocl = clearlist;

    if (!zleactive)
	return 1;
    statusline = NULL;
    statusll = 0;
    if (*args) {
	if (**args)
	    statusline = stringaszleline(*args, 0, &statusll, NULL, NULL);
	if (*++args) {
	    LinkList l = newlinklist();
	    int zmultsav = zmult;

	    for (; *args; args++)
		addlinknode(l, *args);

	    zmult = 1;
	    listlist(l);
	    if (statusline)
		lastlistlen++;
	    showinglist = clearlist = 0;
	    zmult = zmultsav;
	} else if (OPT_ISSET(ops,'c')) {
	    clearlist = 1;
	    lastlistlen = 0;
	}
    } else if (OPT_ISSET(ops,'c')) {
	clearlist = listshown = 1;
	lastlistlen = 0;
    }
    zrefresh();

    if (statusline)
	free(statusline);

    clearlist = ocl;
    statusline = s;
    statusll = sl;
    return 0;
}

/**/
static int
bin_zle_mesg(char *name, char **args, UNUSED(Options ops), UNUSED(char func))
{
    if (!zleactive) {
	zwarnnam(name, "can only be called from widget function", NULL, 0);
	return 1;
    }
    showmsg(*args);
    if (sfcontext != SFC_WIDGET)
	zrefresh();
    return 0;
}

/**/
static int
bin_zle_unget(char *name, char **args, UNUSED(Options ops), UNUSED(char func))
{
    char *b = *args, *p = b + strlen(b);

    if (!zleactive) {
	zwarnnam(name, "can only be called from widget function", NULL, 0);
	return 1;
    }
    while (p > b)
	ungetbyte((int) *--p);
    return 0;
}

/**/
static int
bin_zle_keymap(char *name, char **args, UNUSED(Options ops), UNUSED(char func))
{
    if (!zleactive) {
	zwarnnam(name, "can only be called from widget function", NULL, 0);
	return 1;
    }
    return selectkeymap(*args, 0);
}

/**/
static void
scanlistwidgets(HashNode hn, int list)
{
    Thingy t = (Thingy) hn;
    Widget w = t->widget;

    if(list < 0) {
	printf("%s\n", hn->nam);
	return;
    }
    if(w->flags & WIDGET_INT)
	return;
    if(list) {
	printf("zle -%c ", (w->flags & WIDGET_NCOMP) ? 'C' : 'N');
	if(t->nam[0] == '-')
	    fputs("-- ", stdout);
	quotedzputs(t->nam, stdout);
	if (w->flags & WIDGET_NCOMP) {
	    fputc(' ', stdout);
	    quotedzputs(w->u.comp.wid, stdout);
	    fputc(' ', stdout);
	    quotedzputs(w->u.comp.func, stdout);
	} else if(strcmp(t->nam, w->u.fnnam)) {
	    fputc(' ', stdout);
	    quotedzputs(w->u.fnnam, stdout);
	}
    } else {
	nicezputs(t->nam, stdout);
	if (w->flags & WIDGET_NCOMP) {
	    fputs(" -C ", stdout);
	    nicezputs(w->u.comp.wid, stdout);
	    fputc(' ', stdout);
	    nicezputs(w->u.comp.func, stdout);
	} else if(strcmp(t->nam, w->u.fnnam)) {
	    fputs(" (", stdout);
	    nicezputs(w->u.fnnam, stdout);
	    fputc(')', stdout);
	}
    }
    putchar('\n');
}

/**/
static int
bin_zle_del(char *name, char **args, UNUSED(Options ops), UNUSED(char func))
{
    int ret = 0;

    do {
	Thingy t = (Thingy) thingytab->getnode(thingytab, *args);
	if(!t) {
	    zwarnnam(name, "no such widget `%s'", *args, 0);
	    ret = 1;
	} else if(unbindwidget(t, 0)) {
	    zwarnnam(name, "widget name `%s' is protected", *args, 0);
	    ret = 1;
	}
    } while(*++args);
    return ret;
}

/**/
static int
bin_zle_link(char *name, char **args, UNUSED(Options ops), UNUSED(char func))
{
    Thingy t = (Thingy) thingytab->getnode(thingytab, args[0]);

    if(!t) {
	zwarnnam(name, "no such widget `%s'", args[0], 0);
	return 1;
    } else if(bindwidget(t->widget, rthingy(args[1]))) {
	zwarnnam(name, "widget name `%s' is protected", args[1], 0);
	return 1;
    }
    return 0;

}

/**/
static int
bin_zle_new(char *name, char **args, UNUSED(Options ops), UNUSED(char func))
{
    Widget w = zalloc(sizeof(*w));

    w->flags = 0;
    w->first = NULL;
    w->u.fnnam = ztrdup(args[1] ? args[1] : args[0]);
    if(!bindwidget(w, rthingy(args[0])))
	return 0;
    freewidget(w);
    zwarnnam(name, "widget name `%s' is protected", args[0], 0);
    return 1;
}

/**/
static int
bin_zle_complete(char *name, char **args, UNUSED(Options ops), UNUSED(char func))
{
    Thingy t;
    Widget w, cw;

    if (!require_module(name, "zsh/complete", 0, 0)) {
	zwarnnam(name, "can't load complete module", NULL, 0);
	return 1;
    }
    t = rthingy((args[1][0] == '.') ? args[1] : dyncat(".", args[1]));
    cw = t->widget;
    unrefthingy(t);
    if (!cw || !(cw->flags & ZLE_ISCOMP)) {
	zwarnnam(name, "invalid widget `%s'", args[1], 0);
	return 1;
    }
    w = zalloc(sizeof(*w));
    w->flags = WIDGET_NCOMP|ZLE_MENUCMP|ZLE_KEEPSUFFIX;
    w->first = NULL;
    w->u.comp.fn = cw->u.fn;
    w->u.comp.wid = ztrdup(args[1]);
    w->u.comp.func = ztrdup(args[2]);
    if (bindwidget(w, rthingy(args[0]))) {
	freewidget(w);
	zwarnnam(name, "widget name `%s' is protected", args[0], 0);
	return 1;
    }
    hascompwidgets++;

    return 0;
}

/**/
static int
zle_usable()
{
    return zleactive && !incompctlfunc && !incompfunc
#if 0
	/*
	 * PWS experiment: commenting this out allows zle widgets
	 * in signals, hooks etc.  I'm not sure if this has a down side;
	 * it ought to be that zleactive is good enough to test whether
	 * widgets are callable.
	 */
	&& sfcontext == SFC_WIDGET
#endif
	   ;
}

/**/
static int
bin_zle_call(char *name, char **args, UNUSED(Options ops), UNUSED(char func))
{
    Thingy t;
    struct modifier modsave = zmod;
    int ret, saveflag = 0;
    char *wname = *args++, *keymap_restore = NULL, *keymap_tmp;

    if (!wname)
	return !zle_usable();

    if(!zle_usable()) {
	zwarnnam(name, "widgets can only be called when ZLE is active",
	    NULL, 0);
	return 1;
    }

    UNMETACHECK();

    while (*args && **args == '-') {
	char *num;
	if (!args[0][1] || args[0][1] == '-') {
	    args++;
	    break;
	}
	while (*++(*args)) {
	    switch (**args) {
	    case 'n':
		num = args[0][1] ? args[0]+1 : args[1];
		if (!num) {
		    zwarnnam(name, "number expected after -%c", NULL, **args);
		    return 1;
		}
		if (!args[0][1])
		    *++args = "" - 1;
		saveflag = 1;
		zmod.mult = atoi(num);
		zmod.flags |= MOD_MULT;
		break;
	    case 'N':
		saveflag = 1;
		zmod.mult = 1;
		zmod.flags &= ~MOD_MULT;
		break;
	    case 'K':
		keymap_tmp = args[0][1] ? args[0]+1 : args[1];
		if (!keymap_tmp) {
		    zwarnnam(name, "keymap expected after -%c", NULL, **args);
		    return 1;
		}
		if (!args[0][1])
		    *++args = "" - 1;
		keymap_restore = dupstring(curkeymapname);
		if (selectkeymap(keymap_tmp, 0))
		    return 1;
		break;
	    default:
		zwarnnam(name, "unknown option: %s", *args, 0);
		return 1;
	    }
	}
	args++;
    }

    t = rthingy(wname);
    ret = execzlefunc(t, args);
    unrefthingy(t);
    if (saveflag)
	zmod = modsave;
    if (keymap_restore)
	selectkeymap(keymap_restore, 0);
    return ret;
}

/**/
static int
bin_zle_invalidate(UNUSED(char *name), UNUSED(char **args), UNUSED(Options ops), UNUSED(char func))
{
    /*
     * Trash zle if trashable, but only indicate that zle is usable
     * if it's possible to call a zle widget next.  This is not
     * true if a completion widget is active.
     */
    if (zleactive) {
	if (!trashedzle)
	    trashzle();
	return 0;
    } else
	return 1;
}

/**/
static int
bin_zle_fd(char *name, char **args, Options ops, UNUSED(char func))
{
    int fd = 0, i, found = 0;
    char *endptr;

    if (*args) {
	fd = (int)zstrtol(*args, &endptr, 10);

	if (*endptr || fd < 0) {
	    zwarnnam(name, "Bad file descriptor number for -F: %s", *args, 0);
	    return 1;
	}
    }

    if (OPT_ISSET(ops,'L') || !*args) {
	/* Listing handlers. */
	if (*args && args[1]) {
	    zwarnnam(name, "too many arguments for -FL", NULL, 0);
	    return 1;
	}
	for (i = 0; i < nwatch; i++) {
	    if (*args && watch_fds[i] != fd)
		continue;
	    found = 1;
	    printf("%s -F %d %s\n", name, watch_fds[i], watch_funcs[i]);
	}
	/* only return status 1 if fd given and not found */
	return *args && !found;
    }

    if (args[1]) {
	/* Adding or replacing a handler */
	char *funcnam = ztrdup(args[1]);
	if (nwatch) {
	    for (i = 0; i < nwatch; i++) {
		if (watch_fds[i] == fd) {
		    zsfree(watch_funcs[i]);
		    watch_funcs[i] = funcnam;
		    found = 1;
		    break;
		}
	    }
	}
	if (!found) {
	    /* zrealloc handles NULL pointers, so OK for first time through */
	    int newnwatch = nwatch+1;
	    watch_fds = (int *)zrealloc(watch_fds, 
					newnwatch * sizeof(int));
	    watch_funcs = (char **)zrealloc(watch_funcs,
					    (newnwatch+1) * sizeof(char *));
	    watch_fds[nwatch] = fd;
	    watch_funcs[nwatch] = funcnam;
	    watch_funcs[newnwatch] = NULL;
	    nwatch = newnwatch;
	}
    } else {
	/* Deleting a handler */
	for (i = 0; i < nwatch; i++) {
	    if (watch_fds[i] == fd) {
		int newnwatch = nwatch-1;
		int *new_fds;
		char **new_funcs;

		zsfree(watch_funcs[i]);
		if (newnwatch) {
		    new_fds = zalloc(newnwatch*sizeof(int));
		    new_funcs = zalloc((newnwatch+1)*sizeof(char*));
		    if (i) {
			memcpy(new_fds, watch_fds, i*sizeof(int));
			memcpy(new_funcs, watch_funcs, i*sizeof(char *));
		    }
		    if (i < newnwatch) {
			memcpy(new_fds+i, watch_fds+i+1,
			       (newnwatch-i)*sizeof(int));
			memcpy(new_funcs+i, watch_funcs+i+1,
			       (newnwatch-i)*sizeof(char *));
		    }
		    new_funcs[newnwatch] = NULL;
		} else {
		    new_fds = NULL;
		    new_funcs = NULL;
		}
		zfree(watch_fds, nwatch*sizeof(int));
		zfree(watch_funcs, (nwatch+1)*sizeof(char *));
		watch_fds = new_fds;
		watch_funcs = new_funcs;
		nwatch = newnwatch;
		found = 1;
		break;
	    }
	}
	if (!found) {
	    zwarnnam(name, "No handler installed for fd %d", NULL, fd);
	    return 1;
	}
    }

    return 0;
}

/*******************/
/* initialiasation */
/*******************/

/**/
void
init_thingies(void)
{
    Thingy t;

    createthingytab();
    for(t = thingies; t->nam; t++)
	thingytab->addnode(thingytab, t->nam, t);
}
