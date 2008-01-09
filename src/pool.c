/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * pool.c
 * 
 * The pool contains information about solvables
 * stored optimized for memory consumption and fast retrieval.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "poolid.h"
#include "poolid_private.h"
#include "poolarch.h"
#include "util.h"
#include "bitmap.h"
#include "evr.h"

#define SOLVABLE_BLOCK	255


// list of string constants, so we can do pointer/Id instead of string comparison
// index into array matches ID_xxx constants in pool.h

static const char *initpool_data[] = {
  "<NULL>",                   // ID_NULL
  "",                         // ID_EMPTY
  "solvable:name",
  "solvable:arch",
  "solvable:evr",
  "solvable:vendor",
  "solvable:provides",
  "solvable:obsoletes",
  "solvable:conflicts",
  "solvable:requires",
  "solvable:recommends",
  "solvable:suggests",
  "solvable:supplements",
  "solvable:enhances",
  "solvable:freshens",
  "rpm:dbid",			       /* direct key into rpmdb */
  "solvable:prereqmarker",
  "solvable:filemarker",
  "namespace:installed",
  "namespace:modalias",
  "system:system",
  "src",
  "nosrc",
  "noarch",
  "repodata:external",
  "repodata:keys",
  "repodata:location",
  0
};

/* create pool */
Pool *
pool_create(void)
{
  Pool *pool;
  Solvable *s;

  pool = (Pool *)sat_calloc(1, sizeof(*pool));

  stringpool_init (&pool->ss, initpool_data);

  // pre-alloc space for a RelDep
  pool->rels = (Reldep *)sat_calloc(1 + REL_BLOCK, sizeof(Reldep));
  pool->nrels = 1;

  // pre-alloc space for a Solvable
  pool->solvables = (Solvable *)sat_calloc(SOLVABLE_BLOCK + 1, sizeof(Solvable));
  pool->nsolvables = 2;
  queue_init(&pool->vendormap);
  s = pool->solvables + SYSTEMSOLVABLE;
  s->name = SYSTEM_SYSTEM;
  s->arch = ARCH_NOARCH;
  s->evr = ID_EMPTY;

  pool->debugmask = SAT_DEBUG_RESULT;	/* FIXME */
  return pool;
}


/* free all the resources of our pool */
void
pool_free(Pool *pool)
{
  int i;

  pool_freewhatprovides(pool);
  pool_freeidhashes(pool);
  repo_freeallrepos(pool, 1);
  sat_free(pool->id2arch);
  sat_free(pool->solvables);
  sat_free(pool->ss.stringspace);
  sat_free(pool->ss.strings);
  sat_free(pool->rels);
  queue_free(&pool->vendormap);
  for (i = 0; i < DEP2STRBUF; i++)
    sat_free(pool->dep2strbuf[i]);
  sat_free(pool);
}

Id
pool_add_solvable(Pool *pool)
{
  if ((pool->nsolvables & SOLVABLE_BLOCK) == 0)
    pool->solvables = sat_realloc2(pool->solvables, pool->nsolvables + (SOLVABLE_BLOCK + 1), sizeof(Solvable));
  memset(pool->solvables + pool->nsolvables, 0, sizeof(Solvable));
  return pool->nsolvables++;
}

Id
pool_add_solvable_block(Pool *pool, int count)
{
  Id nsolvables = pool->nsolvables;
  if (!count)
    return nsolvables;
  if (((nsolvables - 1) | SOLVABLE_BLOCK) != ((nsolvables + count - 1) | SOLVABLE_BLOCK))
    pool->solvables = sat_realloc2(pool->solvables, (nsolvables + count + SOLVABLE_BLOCK) & ~SOLVABLE_BLOCK, sizeof(Solvable));
  memset(pool->solvables + nsolvables, 0, sizeof(Solvable) * count);
  pool->nsolvables += count;
  return nsolvables;
}

void
pool_free_solvable_block(Pool *pool, Id start, int count, int reuseids)
{
  if (!count)
    return;
  if (reuseids && start + count == pool->nsolvables)
    {
      /* might want to shrink solvable array */
      pool->nsolvables = start;
      return;
    }
  memset(pool->solvables + start, 0, sizeof(Solvable) * count);
}


const char *
solvable2str(Pool *pool, Solvable *s)
{
  int l, nn = pool->dep2strn;
  const char *n, *e, *a;
  n = id2str(pool, s->name);
  e = id2str(pool, s->evr);
  a = id2str(pool, s->arch);
  l = strlen(n) + strlen(e) + strlen(a) + 3;
  if (l > pool->dep2strlen[nn])
    {
      pool->dep2strbuf[nn] = sat_realloc(pool->dep2strbuf[nn], l + 32);
      pool->dep2strlen[nn] = l + 32;
    }
  sprintf(pool->dep2strbuf[nn], "%s-%s.%s", n, e, a);
  pool->dep2strn = (nn + 1) % DEP2STRBUF;
  return pool->dep2strbuf[nn];
}

static Pool *pool_shrink_whatprovides_sortcmp_data;

static int
pool_shrink_whatprovides_sortcmp(const void *ap, const void *bp)
{
  int r;
  Pool *pool = pool_shrink_whatprovides_sortcmp_data;
  Id oa, ob, *da, *db;
  oa = pool->whatprovides[*(Id *)ap];
  ob = pool->whatprovides[*(Id *)bp];
  if (oa == ob)
    return *(Id *)ap - *(Id *)bp;
  if (!oa)
    return -1;
  if (!ob)
    return 1;
  da = pool->whatprovidesdata + oa;
  db = pool->whatprovidesdata + ob;
  while (*db)
    if ((r = (*da++ - *db++)) != 0)
      return r;
  if (*da)
    return *da;
  return *(Id *)ap - *(Id *)bp;
}

/*
 * pool_shrink_whatprovides  - unify whatprovides data
 *
 * whatprovides_rel must be empty for this to work!
 *
 */
static void
pool_shrink_whatprovides(Pool *pool)
{
  Id i, id;
  Id *sorted;
  Id lastid, *last, *dp, *lp;
  Offset o;
  int r;

  if (pool->ss.nstrings < 3)
    return;
  sorted = sat_malloc2(pool->ss.nstrings, sizeof(Id));
  for (id = 0; id < pool->ss.nstrings; id++)
    sorted[id] = id;
  pool_shrink_whatprovides_sortcmp_data = pool;
  qsort(sorted + 1, pool->ss.nstrings - 1, sizeof(Id), pool_shrink_whatprovides_sortcmp);
  last = 0;
  lastid = 0;
  for (i = 1; i < pool->ss.nstrings; i++)
    {
      id = sorted[i];
      o = pool->whatprovides[id];
      if (o == 0 || o == 1)
	continue;
      dp = pool->whatprovidesdata + o;
      if (last)
	{
	  lp = last;
	  while (*dp)	
	    if (*dp++ != *lp++)
	      {
		last = 0;
	        break;
	      }
	  if (last && *lp)
	    last = 0;
	  if (last)
	    {
	      pool->whatprovides[id] = -lastid;
	      continue;
	    }
	}
      last = pool->whatprovidesdata + o;
      lastid = id;
    }
  sat_free(sorted);
  dp = pool->whatprovidesdata + 2;
  for (id = 1; id < pool->ss.nstrings; id++)
    {
      o = pool->whatprovides[id];
      if (o == 0 || o == 1)
	continue;
      if ((Id)o < 0)
	{
	  i = -(Id)o;
	  if (i >= id)
	    abort();
	  pool->whatprovides[id] = pool->whatprovides[i];
	  continue;
	}
      lp = pool->whatprovidesdata + o;
      if (lp < dp)
	abort();
      pool->whatprovides[id] = dp - pool->whatprovidesdata;
      while ((*dp++ = *lp++) != 0)
	;
    }
  o = dp - pool->whatprovidesdata;
  POOL_DEBUG(SAT_DEBUG_STATS, "shrunk whatprovidesdata from %d to %d\n", pool->whatprovidesdataoff, o);
  if (pool->whatprovidesdataoff == o)
    return;
  r = pool->whatprovidesdataoff - o;
  pool->whatprovidesdataoff = o;
  pool->whatprovidesdata = sat_realloc(pool->whatprovidesdata, (o + pool->whatprovidesdataleft) * sizeof(Id));
  if (r > pool->whatprovidesdataleft)
    r = pool->whatprovidesdataleft;
  memset(pool->whatprovidesdata + o, 0, r * sizeof(Id));
}


/*
 * pool_createwhatprovides()
 * 
 * create hashes over pool of solvables to ease provide lookups
 * 
 */
void
pool_createwhatprovides(Pool *pool)
{
  int i, num, np, extra;
  Offset off;
  Solvable *s;
  Id id;
  Offset *idp, n;
  Offset *whatprovides;
  Id *whatprovidesdata, *d;

  POOL_DEBUG(SAT_DEBUG_STATS, "number of solvables: %d\n", pool->nsolvables);
  POOL_DEBUG(SAT_DEBUG_STATS, "number of ids: %d + %d\n", pool->ss.nstrings, pool->nrels);

  pool_freeidhashes(pool);	/* XXX: should not be here! */
  pool_freewhatprovides(pool);
  num = pool->ss.nstrings;
  pool->whatprovides = whatprovides = sat_calloc((num + WHATPROVIDES_BLOCK) & ~WHATPROVIDES_BLOCK, sizeof(Offset));
  pool->whatprovides_rel = sat_calloc((pool->nrels + WHATPROVIDES_BLOCK) & ~WHATPROVIDES_BLOCK, sizeof(Offset));

  /* count providers for each name */
  for (i = 1; i < pool->nsolvables; i++)
    {
      Id *pp;
      s = pool->solvables + i;
      if (!s->provides)
	continue;
      if (!pool_installable(pool, s))
	continue;
      pp = s->repo->idarraydata + s->provides;
      while ((id = *pp++) != ID_NULL)
	{
	  while (ISRELDEP(id))
	    {
	      Reldep *rd = GETRELDEP(pool, id);
	      id = rd->name;
	    }
	  whatprovides[id]++;	       /* inc count of providers */
	}
    }

  off = 2;	/* first entry is undef, second is empty list */
  idp = whatprovides;
  np = 0;			       /* number of names provided */
  for (i = 0; i < num; i++, idp++)
    {
      n = *idp;
      if (!n)			       /* no providers */
	continue;
      *idp = off;		       /* move from counts to offsets into whatprovidesdata */
      off += n + 1;		       /* make space for all providers + terminating ID_NULL */
      np++;			       /* inc # of provider 'slots' */
    }

  POOL_DEBUG(SAT_DEBUG_STATS, "provide ids: %d\n", np);

  /* reserve some space for relation data */
  extra = 2 * pool->nrels;
  if (extra < 256)
    extra = 256;

  POOL_DEBUG(SAT_DEBUG_STATS, "provide space needed: %d + %d\n", off, extra);

  /* alloc space for all providers + extra */
  whatprovidesdata = sat_calloc(off + extra, sizeof(Id));

  /* now fill data for all provides */
  for (i = 1; i < pool->nsolvables; i++)
    {
      Id *pp;
      s = pool->solvables + i;
      if (!s->provides)
	continue;
      if (!pool_installable(pool, s))
	continue;

      /* for all provides of this solvable */
      pp = s->repo->idarraydata + s->provides;
      while ((id = *pp++) != 0)
	{
	  while (ISRELDEP(id))
	    {
	      Reldep *rd = GETRELDEP(pool, id);
	      id = rd->name;
	    }
	  d = whatprovidesdata + whatprovides[id];   /* offset into whatprovidesdata */
	  if (*d)
	    {
	      d++;
	      while (*d)	       /* find free slot */
		d++;
	      if (d[-1] == i)
		continue;
	    }
	  *d = i;		       /* put solvable Id into data */
	}
    }
  pool->whatprovidesdata = whatprovidesdata;
  pool->whatprovidesdataoff = off;
  pool->whatprovidesdataleft = extra;
  pool_shrink_whatprovides(pool);
}

/*
 * free all of our whatprovides data
 * be careful, everything internalized with pool_queuetowhatprovides is gone, too
 */
void
pool_freewhatprovides(Pool *pool)
{
  pool->whatprovides = sat_free(pool->whatprovides);
  pool->whatprovides_rel = sat_free(pool->whatprovides_rel);
  pool->whatprovidesdata = sat_free(pool->whatprovidesdata);
  pool->whatprovidesdataoff = 0;
  pool->whatprovidesdataleft = 0;
}


/******************************************************************************/

/*
 * pool_queuetowhatprovides  - add queue contents to whatprovidesdata
 * 
 * on-demand filling of provider information
 * move queue data into whatprovidesdata
 * q: queue of Ids
 * returns: Offset into whatprovides
 *
 */
Id
pool_queuetowhatprovides(Pool *pool, Queue *q)
{
  Offset off;
  int count = q->count;

  if (count == 0)		       /* queue empty -> ID_EMPTY */
    return ID_EMPTY;

  /* extend whatprovidesdata if needed, +1 for ID_NULL-termination */
  if (pool->whatprovidesdataleft < count + 1)
    {
      POOL_DEBUG(SAT_DEBUG_STATS, "growing provides hash data...\n");
      pool->whatprovidesdata = sat_realloc(pool->whatprovidesdata, (pool->whatprovidesdataoff + count + 4096) * sizeof(Id));
      pool->whatprovidesdataleft = count + 4096;
    }

  /* copy queue to next free slot */
  off = pool->whatprovidesdataoff;
  memcpy(pool->whatprovidesdata + pool->whatprovidesdataoff, q->elements, count * sizeof(Id));

  /* adapt count and ID_NULL-terminate */
  pool->whatprovidesdataoff += count;
  pool->whatprovidesdata[pool->whatprovidesdataoff++] = ID_NULL;
  pool->whatprovidesdataleft -= count + 1;

  return (Id)off;
}


/*************************************************************************/

/*
 * addrelproviders
 * 
 * add packages fulfilling the relation to whatprovides array
 * no exact providers, do range match
 * 
 */

Id *
pool_addrelproviders(Pool *pool, Id d)
{
  Reldep *rd = GETRELDEP(pool, d);
  Reldep *prd;
  Queue plist;
  Id buf[16];
  Id name = rd->name;
  Id evr = rd->evr;
  int flags = rd->flags;
  Id pid, *pidp;
  Id p, *pp, *pp2, *pp3;

  d = GETRELID(d);
  queue_init_buffer(&plist, buf, sizeof(buf)/sizeof(*buf));
  switch (flags)
    {
    case REL_AND:
    case REL_WITH:
      pp = pool_whatprovides(pool, name);
      pp2 = pool_whatprovides(pool, evr);
      while ((p = *pp++) != 0)
	{
	  for (pp3 = pp2; *pp3;)
	    if (*pp3++ == p)
	      {
	        queue_push(&plist, p);
		break;
	      }
	}
      break;
    case REL_OR:
      pp = pool_whatprovides(pool, name);
      while ((p = *pp++) != 0)
	queue_push(&plist, p);
      pp = pool_whatprovides(pool, evr);
      while ((p = *pp++) != 0)
	queue_pushunique(&plist, p);
      break;
    case REL_NAMESPACE:
      if (pool->nscallback)
	{
	  p = pool->nscallback(pool, pool->nscallbackdata, name, evr);
	  if (p > 1)
	    {
	      queue_free(&plist);
	      pool->whatprovides_rel[d] = p;
	      return pool->whatprovidesdata + p;
	    }
	  if (p == 1)
	    queue_push(&plist, SYSTEMSOLVABLE);
	}
      break;
    default:
      break;
    }

  /* convert to whatprovides id */
#if 0
  POOL_DEBUG(DEBUG_1, "addrelproviders: what provides %s?\n", id2str(pool, name));
#endif
  if (flags && flags < 8)
    {
      FOR_PROVIDES(p, pp, name)
	{
#if 0
	  POOL_DEBUG(DEBUG_1, "addrelproviders: checking package %s\n", id2str(pool, pool->p[p].name));
#endif
	  /* solvable p provides name in some rels */
	  pidp = pool->solvables[p].repo->idarraydata + pool->solvables[p].provides;
	  while ((pid = *pidp++) != 0)
	    {
	      int pflags;
	      Id pevr;

	      if (pid == name)
		{
#ifdef DEBIAN_SEMANTICS
		  continue;		/* unversioned provides can
				 	 * never match versioned deps */
#else
		  break;		/* yes, provides all versions */
#endif
		}
	      if (!ISRELDEP(pid))
		continue;		/* wrong provides name */
	      prd = GETRELDEP(pool, pid);
	      if (prd->name != name)
		continue;		/* wrong provides name */
	      /* right package, both deps are rels */
	      pflags = prd->flags;
	      if (!pflags)
		continue;
	      if (flags == 7 || pflags == 7)
		break; /* included */
	      if ((pflags & flags & 5) != 0)
		break; /* same direction, match */
	      pevr = prd->evr;
	      if (pevr == evr)
		{
		  if ((pflags & flags & 2) != 0)
		    break; /* both have =, match */
		}
	      else
		{
		  int f = flags == 5 ? 5 : flags == 2 ? pflags : (flags ^ 5) & (pflags | 5);
		  if ((f & (1 << (1 + evrcmp(pool, pevr, evr, EVRCMP_MATCH_RELEASE)))) != 0)
		    break;
		}
	    }
	  if (!pid)
	    continue;	/* no rel match */
	  queue_push(&plist, p);
	}
      /* make our system solvable provide all unknown rpmlib() stuff */
      if (plist.count == 0 && !strncmp(id2str(pool, name), "rpmlib(", 7))
	queue_push(&plist, SYSTEMSOLVABLE);
    }
  /* add providers to whatprovides */
#if 0
  POOL_DEBUG(DEBUG_1, "addrelproviders: adding %d packages to %d\n", plist.count, d);
#endif
  pool->whatprovides_rel[d] = pool_queuetowhatprovides(pool, &plist);
  queue_free(&plist);

  return pool->whatprovidesdata + pool->whatprovides_rel[d];
}

/*************************************************************************/

void
pool_debug(Pool *pool, int type, const char *format, ...)
{
  va_list args;
  char buf[1024];

  if ((type & SAT_FATAL) == 0)
    {
      if ((pool->debugmask & type) == 0)
	return;
    }
  va_start(args, format);
  if (!pool->debugcallback)
    {
      if ((type & (SAT_FATAL|SAT_ERROR)) == 0)
        vprintf(format, args);
      else
        vfprintf(stderr, format, args);
      return;
    }
  vsnprintf(buf, sizeof(buf), format, args);
  pool->debugcallback(pool, pool->debugcallbackdata, type, buf);
}

void
pool_setdebuglevel(Pool *pool, int level)
{
  int mask = SAT_DEBUG_RESULT;
  if (level > 0)
    mask |= SAT_DEBUG_STATS|SAT_DEBUG_ANALYZE|SAT_DEBUG_UNSOLVABLE;
  if (level > 1)
    mask |= SAT_DEBUG_JOB|SAT_DEBUG_SOLUTIONS|SAT_DEBUG_POLICY;
  if (level > 2)
    mask |= SAT_DEBUG_PROPAGATE;
  if (level > 3)
    mask |= SAT_DEBUG_RULE_CREATION;
  if (level > 4)
    mask |= SAT_DEBUG_SCHUBI;
  pool->debugmask = mask;
}

/*************************************************************************/

struct searchfiles {
  const char **files;
  int nfiles;
};

#define SEARCHFILES_BLOCK 127

static void
pool_addfileprovides_dep(Pool *pool, Id *ida, Map *seen, struct searchfiles *sf)
{
  Id dep, sid;
  const char *s;

  while ((dep = *ida++) != 0)
    {
      while (ISRELDEP(dep))
	{
	  Reldep *rd;
	  sid = pool->ss.nstrings + GETRELID(dep);
	  if (MAPTST(seen, sid))
	    {
	      dep = 0;
	      break;
	    }
	  MAPSET(seen, sid);
	  rd = GETRELDEP(pool, dep);
	  if (rd->flags < 8)
	    dep = rd->name;
	  else if (rd->flags == REL_NAMESPACE)
	    {
	      if (rd->name == NAMESPACE_INSTALLED)
		{
		  dep = 0;	/* for now */
		  break;
		}
	      dep = rd->evr;
	    }
	  else
	    {
	      Id ids[2];
	      ids[0] = rd->name;
	      ids[1] = 0;
	      pool_addfileprovides_dep(pool, ids, seen, sf);
	      dep = rd->evr;
	    }
	}
      if (!dep)
	continue;
      if (MAPTST(seen, dep))
	continue;
      MAPSET(seen, dep);
      s = id2str(pool, dep);
      if (*s != '/')
	continue;
      if ((sf->nfiles & SEARCHFILES_BLOCK) == 0)
	sf->files = sat_realloc2(sf->files, sf->nfiles + (SEARCHFILES_BLOCK + 1), sizeof(const char *));
      sf->files[sf->nfiles++] = strdup(s);
    }
}

#if 0
static int
addfileprovides_cb(void *data, Solvable *s, Id key, const char *str)
{
  Pool *pool = s->repo->pool;
  Id id;
  id = str2id(pool, str, 0);
  if (!id)
    return 0;	/* can't happen */
  s->provides = repo_addid_dep(s->repo, s->provides, id, SOLVABLE_FILEMARKER);
  return 0;
}
#endif

void
pool_addfileprovides(Pool *pool)
{
  Solvable *s;
  Repo *repo;
  Map seen;
  struct searchfiles sf;
  int i;

  map_init(&seen, pool->ss.nstrings + pool->nrels);
  memset(&sf, 0, sizeof(sf));

  for (i = 1, s = pool->solvables + i; i < pool->nsolvables; i++, s++)
    {
      repo = s->repo;
      if (!repo)
	continue;
      if (s->obsoletes)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->obsoletes, &seen, &sf);
      if (s->conflicts)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->conflicts, &seen, &sf);
      if (s->requires)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->requires, &seen, &sf);
      if (s->recommends)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->recommends, &seen, &sf);
      if (s->suggests)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->suggests, &seen, &sf);
      if (s->supplements)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->supplements, &seen, &sf);
      if (s->enhances)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->enhances, &seen, &sf);
      if (s->freshens)
        pool_addfileprovides_dep(pool, repo->idarraydata + s->freshens, &seen, &sf);
    }
  map_free(&seen);
  POOL_DEBUG(SAT_DEBUG_STATS, "found %d file dependencies\n", sf.nfiles);
  if (!sf.nfiles)
    return;
#if 0
  for (i = 0; i < sf.nfiles; i++)
    POOL_DEBUG(SAT_DEBUG_STATS, "looking up %s in filelist\n", sf.files[i]);
#endif
  if ((sf.nfiles & SEARCHFILES_BLOCK) == 0)
    sf.files = sat_realloc2(sf.files, sf.nfiles + (SEARCHFILES_BLOCK + 1), sizeof(const char *));
  sf.files[sf.nfiles++] = 0;
#if 0
  pool_search(0, SOLVABLE_FILELIST, (const char *)sf.files, SEARCH_STRING|SEARCH_MULTIPLE, addfileprovides_cb, 0);
#endif
  sat_free(sf.files);
  pool_freewhatprovides(pool);	/* as we have added provides */
}

// EOF
