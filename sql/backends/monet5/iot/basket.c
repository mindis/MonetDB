/*
 * The contents of this file are subject to the MonetDB Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.monetdb.org/Legal/MonetDBLicense
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is the MonetDB Database System.
 *
 * The Initial Developer of the Original Code is CWI.
 * Portions created by CWI are Copyright (C) 1997-July 2008 CWI.
 * Copyright August 2008-2016 MonetDB B.V.
 * All Rights Reserved.
 */

/* author: M. Kersten
 * Continuous query processing relies on event baskets
 * passed through a processing pipeline. The baskets
 * are derived from ordinary SQL tables where the delta
 * processing is ignored.
 *
 */

#include "monetdb_config.h"
#include "gdk.h"
#include "iot.h"
#include "basket.h"
#include "mal_exception.h"
#include "mal_builder.h"
#include "opt_prelude.h"

#define _DEBUG_BASKET_ if(0)

str statusname[4] = { "<unknown>", "available", "wait", "locked" };

BasketRec *baskets;   /* the global iot catalog */
static int bsktTop = 0, bsktLimit = 0;

// Find an empty slot in the basket catalog
static int BSKTnewEntry(void)
{
	int i;
	if (bsktLimit == 0) {
		bsktLimit = MAXBSKT;
		baskets = (BasketRec *) GDKzalloc(bsktLimit * sizeof(BasketRec));
		bsktTop = 1; /* entry 0 is used as non-initialized */
	} else if (bsktTop +1 == bsktLimit) {
		bsktLimit += MAXBSKT;
		baskets = (BasketRec *) GDKrealloc(baskets, bsktLimit * sizeof(BasketRec));
	}
	for (i = 1; i < bsktLimit; i++)
		if (baskets[i].table_name == NULL)
			break;
	MT_lock_init(&baskets[i].lock,"bsktlock");

	bsktTop++;
	return i;
}


// free all malloced space
static void
BSKTclean(int idx)
{
	GDKfree(baskets[idx].schema_name);
	GDKfree(baskets[idx].table_name);
	baskets[idx].schema_name = NULL;
	baskets[idx].table_name = NULL;

	BBPreclaim(baskets[idx].errors);
	baskets[idx].errors = NULL;
	baskets[idx].count = 0;
}

// locate the basket in the catalog
int
BSKTlocate(str sch, str tbl)
{
	int i;

	if( sch == 0 || tbl == 0)
		return 0;
	for (i = 1; i < bsktTop; i++)
		if (baskets[i].schema_name && strcmp(sch, baskets[i].schema_name) == 0 &&
			baskets[i].table_name && strcmp(tbl, baskets[i].table_name) == 0)
			return i;
	return 0;
}

// Instantiate a basket description for a particular table
static str
BSKTnewbasket(sql_schema *s, sql_table *t)
{
	int i, idx;
	int colcnt=0;
	node *o;

	// Don't introduce the same basket twice
	if( BSKTlocate(s->base.name, t->base.name) > 0)
		return MAL_SUCCEED;
	MT_lock_set(&iotLock);
	idx = BSKTnewEntry();

	baskets[idx].schema_name = GDKstrdup(s->base.name);
	baskets[idx].table_name = GDKstrdup(t->base.name);
	baskets[idx].seen = * timestamp_nil;

	baskets[idx].status = BSKTWAIT;
	baskets[idx].count = 0;
	for (o = t->columns.set->h; o; o = o->next){
        sql_column *col = o->data;
        int tpe = col->type.type->localtype;

        if ( !(tpe <= TYPE_str || tpe == TYPE_date || tpe == TYPE_daytime || tpe == TYPE_timestamp) ){
			MT_lock_unset(&iotLock);
			throw(MAL,"baskets.register","Unsupported type %d",tpe);
		}
		colcnt++;
	}
	// collect the column names
	baskets[idx].cols = (str*) GDKzalloc(sizeof(str) * (colcnt+1));
	for (i=0, o = t->columns.set->h; o; o = o->next){
        sql_column *col = o->data;
		baskets[idx].cols[i++]=  col->base.name;
	}
	
	//
	baskets[idx].errors = BATnew(TYPE_void, TYPE_str, BATTINY, TRANSIENT);
	if (baskets[idx].table_name == NULL ||
	    baskets[idx].errors == NULL) {
		BSKTclean(idx);
		MT_lock_unset(&iotLock);
		throw(MAL,"baskets.register",MAL_MALLOC_FAIL);
	}

	baskets[idx].schema = s;
	baskets[idx].table = t;
	MT_lock_unset(&iotLock);
	return MAL_SUCCEED;
}

// MAL/SQL interface for registration of a single table
static str
BSKTregisterInternal(Client cntxt, MalBlkPtr mb, str sch, str tbl)
{
	sql_schema  *s;
	sql_table   *t;
	mvc *m = NULL;
	str msg = getSQLContext(cntxt, mb, &m, NULL);

	if ( msg != MAL_SUCCEED)
		return msg;

	/* check double registration */
	if( BSKTlocate(sch, tbl) > 0)
		return msg;
	if ((msg = checkSQLContext(cntxt)) != MAL_SUCCEED)
		return msg;

	s = mvc_bind_schema(m, sch);
	if (s == NULL)
		throw(SQL, "iot.register", "Schema missing");

	t = mvc_bind_table(m, s, tbl);
	if (t == NULL)
		throw(SQL, "iot.register", "Table missing '%s'", tbl);

	msg=  BSKTnewbasket(s, t);
	return msg;
}

str
BSKTregister(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	str sch, tbl;

	if( stk == 0){
		sch = getVarConstant(mb, getArg(pci,1)).val.sval;
		tbl = getVarConstant(mb, getArg(pci,2)).val.sval;
	} else{
		sch = *getArgReference_str(stk, pci, 1);
		tbl = *getArgReference_str(stk, pci, 2);
	}
	return BSKTregisterInternal(cntxt,mb,sch,tbl);
}

str
BSKTactivate(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	str sch, tbl;
	int idx = 0;

	(void) cntxt;
	(void) mb;

	if( pci->argc > pci->retc){
		sch = *getArgReference_str(stk, pci, 1);
		tbl = *getArgReference_str(stk, pci, 2);

		/* check for registration */
		idx = BSKTlocate(sch, tbl);
		if( idx == 0)
			throw(SQL,"basket.activate","Stream table %s.%s not accessible to activate\n",sch,tbl);
		if( baskets[idx].status == BSKTWAIT){
			MT_lock_set(&iotLock);
			baskets[idx].status = BSKTAVAILABLE;
			MT_lock_unset(&iotLock);
		}
	} else {
		MT_lock_set(&iotLock);
		for( idx =1; idx <bsktTop;  idx++)
			baskets[idx].status = BSKTAVAILABLE;
		MT_lock_unset(&iotLock);
	}
	return MAL_SUCCEED;
}

str
BSKTdeactivate(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	str sch, tbl;
	int idx = 0;

	(void) cntxt;
	(void) mb;
	if( pci->argc > pci->retc){
		sch = *getArgReference_str(stk, pci, 1);
		tbl = *getArgReference_str(stk, pci, 2);

		/* check for registration */
		idx = BSKTlocate(sch, tbl);
		if( idx == 0)
			throw(SQL,"basket.activate","Stream table %s.%s not accessible to deactivate\n",sch,tbl);
		if( baskets[idx].status == BSKTAVAILABLE ){
			MT_lock_set(&iotLock);
			baskets[idx].status = BSKTWAIT;
			MT_lock_unset(&iotLock);
		}
	} else {
		MT_lock_set(&iotLock);
		for( idx =1; idx <bsktTop;  idx++)
			baskets[idx].status = BSKTWAIT;
		MT_lock_unset(&iotLock);
	}
	return MAL_SUCCEED;
}

str
BSKTthreshold(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	str sch = *getArgReference_str(stk,pci,1);
	str tbl = *getArgReference_str(stk,pci,2);
	int elm = *getArgReference_int(stk,pci,3);
	int idx;

	(void) cntxt;
	(void) mb;

	if( elm < 0)
		throw(SQL,"basket.beat","Positive number of elements expected]n");
	idx = BSKTlocate(sch, tbl);
	if( idx == 0){
		BSKTregisterInternal(cntxt, mb, sch, tbl);
		idx = BSKTlocate(sch, tbl);
		if( idx ==0)
			throw(SQL,"basket.threshold","Stream table %s.%s not accessible to deactivate\n",sch,tbl);
	}
	baskets[idx].threshold = elm;
	return MAL_SUCCEED;
}

str
BSKTbeat(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	str sch = *getArgReference_str(stk,pci,1);
	str tbl = *getArgReference_str(stk,pci,2);
	int ticks = *getArgReference_int(stk,pci,3);
	int idx;

	(void) cntxt;
	(void) mb;

	if( ticks < 0)
		throw(SQL,"basket.beat","Positive beat expected]n");
	idx = BSKTlocate(sch, tbl);
	if( idx == 0){
		BSKTregisterInternal(cntxt, mb, sch, tbl);
		idx = BSKTlocate(sch, tbl);
		if( idx ==0)
			throw(SQL,"basket.beat","Stream table %s.%s not accessible to deactivate\n",sch,tbl);
	}
	baskets[idx].beat = ticks;
	return MAL_SUCCEED;
}

str
BSKTwindow(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	str sch = *getArgReference_str(stk,pci,1);
	str tbl = *getArgReference_str(stk,pci,2);
	int elm = *getArgReference_int(stk,pci,3);
	int idx;

	(void) cntxt;
	(void) mb;
	if( elm < 0)
		throw(SQL,"basket.window","Positive beat expected]n");
	idx = BSKTlocate(sch, tbl);
	if( idx == 0){
		BSKTregisterInternal(cntxt, mb, sch, tbl);
		idx = BSKTlocate(sch, tbl);
		if( idx ==0)
			throw(SQL,"basket.window","Stream table %s.%s not accessible to deactivate\n",sch,tbl);
	}
	baskets[idx].winsize = elm;
	baskets[idx].winstride = elm;
	if ( pci->argc == 5)
		baskets[idx].winstride = *getArgReference_int(stk,pci,4);
	return MAL_SUCCEED;
}

static BAT *
BSKTbindColumn(Client cntxt, str sch, str tbl, str col)
{
	BAT *b = NULL;
	mvc *m = NULL;
	sql_schema *s = NULL;
	sql_table *t = NULL;
	sql_column *c = NULL;

	if( BSKTlocate(sch,tbl) < 0)
		return b;

	if( getSQLContext(cntxt,NULL, &m, NULL) )
		return 0;
	s= mvc_bind_schema(m, sch);
	if ( s)
		t= mvc_bind_table(m, s, tbl);
	if ( t)
		c= mvc_bind_column(m, t, col);


	if( c)
		b = store_funcs.bind_col(m->session->tr,c,RD_INS);
	return b;
}

str
BSKTbind(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	bat *ret = getArgReference_bat(stk,pci,0);
	str sch = *getArgReference_str(stk,pci,1);
	str tbl = *getArgReference_str(stk,pci,2);
	str col = *getArgReference_str(stk,pci,3);

	BAT *b = BSKTbindColumn(cntxt, sch,tbl,col);
	if( b){
		BBPkeepref(*ret =  b->batCacheid);
		return MAL_SUCCEED;
	}
	(void) mb;
	throw(SQL,"iot.bind","Stream table column '%s.%s.%s' not found",sch,tbl,col);
}

str
BSKTdrop(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	int bskt;
	str sch= *getArgReference_str(stk,pci,1);
	str tbl= *getArgReference_str(stk,pci,2);

	(void) cntxt;
	(void) mb;
	bskt = BSKTlocate(sch,tbl);
	if (bskt == 0)
		throw(SQL, "basket.drop", "Could not find the basket %s.%s",sch,tbl);
	MT_lock_set(&iotLock);
	BSKTclean(bskt);
	MT_lock_unset(&iotLock);
	return MAL_SUCCEED;
}

str
BSKTreset(void *ret)
{
	int i;
	(void) ret;
	MT_lock_set(&iotLock);
	for (i = 1; i < bsktLimit; i++)
		if (baskets[i].table_name)
			BSKTclean(i);
	MT_lock_unset(&iotLock);
	return MAL_SUCCEED;
}

/* collect the binary files and append them to what we have */
#define MAXLINE 4096
str
BSKTimportInternal(int bskt)
{
	char buf[PATHLENGTH];
	node *n;
	mvc *m = NULL;
	BAT *b;
	int first=1,i;
	BUN cnt =0, bcnt=0;
	str msg= MAL_SUCCEED;
	FILE *f;
	long fsize;
	char line[MAXLINE];
	str dir = baskets[bskt].source;

	// check access permission to directory first
	if( access (dir , F_OK | R_OK)){
		throw(SQL, "iot.basket", "Could not access the basket directory %s. error %d",dir,errno);
	}
	
	/* check for missing files */
	for( n = baskets[bskt].table->columns.set->h; n; n= n->next){
		sql_column *c = n->data;
		snprintf(buf,PATHLENGTH, "%s%c%s",dir,DIR_SEP, c->base.name);
		_DEBUG_BASKET_ mnstr_printf(BSKTout,"Attach the file %s\n",buf);
		if( access (buf,R_OK))
			throw(MAL,"iot.basket","Could not access the column %s file %s\n",c->base.name, buf);
		b = store_funcs.bind_col(m->session->tr,c,RD_INS);
		if( b == 0)
			throw(MAL,"iot.basket","Could not access the column %s\n",c->base.name);
	}

	// types are already checked during stream initialization
	MT_lock_set(&iotLock);
	for( n = baskets[bskt].table->columns.set->h; msg == MAL_SUCCEED && n; n= n->next){
		sql_column *c = n->data;
		snprintf(buf,PATHLENGTH, "%s%c%s",dir,DIR_SEP, c->base.name);
		_DEBUG_BASKET_ mnstr_printf(BSKTout,"Attach the file %s\n",buf);
		f=  fopen(buf,"r");
		if( f == NULL){
			msg= createException(MAL,"iot.basket","Could not access the column %s file %s\n",c->base.name, buf);
			break;
		}
		(void) fseek(f,0, SEEK_END);
		fsize = ftell(f);
		rewind(f);
		b = store_funcs.bind_col(m->session->tr,c,RD_INS);
		assert( b);
		bcnt = BATcount(b);

		switch(ATOMstorage(b->ttype)){
		case TYPE_bit:
		case TYPE_bte:
		case TYPE_sht:
		case TYPE_int:
		case TYPE_void:
		case TYPE_oid:
		case TYPE_flt:
		case TYPE_dbl:
		case TYPE_lng:
#ifdef HAVE_HGE
		case TYPE_hge:
#endif
			if( BATextend(b, bcnt + fsize / ATOMsize(b->ttype)) != GDK_SUCCEED){
				BBPunfix(b->batCacheid);
				(void) fclose(f);
				msg= createException(MAL,"iot.basket","Could not extend basket %s\n",c->base.name);
				goto recover;
			}
			/* append the binary partition */
			if( fread(Tloc(b, BUNlast(b)),1,fsize, f) != (size_t) fsize){
				BBPunfix(b->batCacheid);
				(void) fclose(f);
				msg= createException(MAL,"iot.basket","Could not read complete basket file %s\n",c->base.name);
				goto recover;
			}
			BATsetcount(b, bcnt + fsize/ ATOMsize(b->ttype));
		break;
		case TYPE_str:
			while (fgets(line, MAXLINE, f) != 0){ //Use getline? http://man7.org/linux/man-pages/man3/getline.3.html
				if ( line[i= strlen(line)-1] != '\n')
					msg= createException(MAL,"iot.basket","string too long\n");
				else{
					line[i] = 0;
					BUNappend(b, line, TRUE);
					bcnt++;
				}
			}
			BATsetcount(b, bcnt );
		}
		(void) fclose(f);
	}

	/* check for mis-aligned columns */
	for( n = baskets[bskt].table->columns.set->h; msg == MAL_SUCCEED && n; n= n->next){
		sql_column *c = n->data;
		b = store_funcs.bind_col(m->session->tr,c,RD_INS);
		assert( b );
		if( first){
			first = 0;
			cnt = BATcount(b);
		} else
		if( cnt != BATcount(b))
			msg= createException(MAL,"iot.basket","Columns mis-aligned %s\n",c->base.name);
		BBPunfix(b->batCacheid);
	}
	/* remove the basket files */
	for( n = baskets[bskt].table->columns.set->h; n; n= n->next){
		sql_column *c = n->data;
		snprintf(buf,PATHLENGTH, "%s%c%s",dir,DIR_SEP, c->base.name);
		assert( access (buf,R_OK) == 0);
		//unlink(buf);
	}
	baskets[bskt].status = BSKTAVAILABLE;
	baskets[bskt].count = cnt;

recover:
	/* reset all BATs when they are misaligned or error occurred */
	if( msg != MAL_SUCCEED)
	for( n = baskets[bskt].table->columns.set->h; n; n= n->next){
		sql_column *c = n->data;
		b = store_funcs.bind_col(m->session->tr,c,RD_INS);
		assert( b );
		BATsetcount(b,0);
		BBPunfix(b->batCacheid);
	}

	MT_lock_unset(&iotLock);
    return msg;
}

str 
BSKTimport(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
    str sch = *getArgReference_str(stk, pci, 1);
    str tbl = *getArgReference_str(stk, pci, 2);
    str dir = *getArgReference_str(stk, pci, 3);
    int bskt;
	str msg= MAL_SUCCEED;
	mvc *m = NULL;

	msg= getSQLContext(cntxt,NULL, &m, NULL);
	if( msg != MAL_SUCCEED)
		return msg;
	BSKTregisterInternal(cntxt, mb, sch, tbl);
    bskt = BSKTlocate(sch,tbl);
	if (bskt == 0)
		throw(SQL, "iot.basket", "Could not find the basket %s.%s",sch,tbl);
	baskets[bskt].source = GDKstrdup(dir);
	return BSKTimportInternal(bskt);
}

/* remove tuples from a basket according to the sliding policy */
#define ColumnShift(B,TPE, STRIDE) { \
	TPE *first= (TPE*) Tloc(B, BUNfirst(B));\
	TPE *n = first+STRIDE;\
	TPE *last=  (TPE*) Tloc(B, BUNlast(B));\
	for( ; n < last; n++, first++)\
		*first=*n;\
}

static str
BSKTtumbleInternal(Client cntxt, str sch, str tbl, int stride)
{
	BAT *b;
	BUN cnt;
	node *n;
	mvc *m = NULL;
	str msg;
	int bskt;

	msg= getSQLContext(cntxt,NULL, &m, NULL);
	if( msg )
		throw(SQL,"iot.tumble","Missing SQL context");
    bskt = BSKTlocate(sch,tbl);
	if (bskt == 0)
		throw(SQL, "iot.tumble", "Could not find the basket %s.%s",sch,tbl);

	for( n = baskets[bskt].table->columns.set->h; n; n= n->next){
		sql_column *c = n->data;
		b = store_funcs.bind_col(m->session->tr,c,RD_INS);
		assert( b );
		cnt=BATcount(b);
		if( stride == -1)
			stride = (int) cnt;

		switch(ATOMstorage(b->ttype)){
		case TYPE_bit:ColumnShift(b,bit,stride); break;
		case TYPE_bte:ColumnShift(b,bte,stride); break;
		case TYPE_sht:ColumnShift(b,sht,stride); break;
		case TYPE_int:ColumnShift(b,int,stride); break;
		case TYPE_oid:ColumnShift(b,oid,stride); break;
		case TYPE_flt:ColumnShift(b,flt,stride); break;
		case TYPE_dbl:ColumnShift(b,dbl,stride); break;
		case TYPE_lng:ColumnShift(b,lng,stride); break;
#ifdef HAVE_HGE
		case TYPE_hge:ColumnShift(b,hge,stride); break;
#endif
		case TYPE_str:
			switch(b->T->width){
			case 1: ColumnShift(b,bte,stride); break;
			case 2: ColumnShift(b,sht,stride); break;
			case 4: ColumnShift(b,int,stride); break;
			case 8: ColumnShift(b,lng,stride); break;
			}
				break;
		default: break;
		}
		if( stride == -1)
			BATsetcount(b, 0);
		else BATsetcount(b, BATcount(b)-stride);
		BBPunfix(b->batCacheid);
	}
	return msg;
}

str
BSKTtumble(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	str sch;
	str tbl;
	int elm = -1;
	int idx;

	(void) cntxt;
	(void) mb;

	sch = *getArgReference_str(stk,pci,1);
	tbl = *getArgReference_str(stk,pci,2);
	if( pci->argc ==4){
		/* clear a named stream  partially*/
		elm = *getArgReference_int(stk,pci,3);
		if( elm < 0)
			throw(SQL,"basket.tumble","Positive slide value expected");
	}

	idx = BSKTlocate(sch, tbl);
	if( idx == 0){
		BSKTregisterInternal(cntxt, mb, sch, tbl);
		idx = BSKTlocate(sch, tbl);
		if( idx ==0)
			throw(SQL,"basket.window","Stream table %s.%s not accessible to deactivate\n",sch,tbl);
	}
	return BSKTtumbleInternal(cntxt, sch, tbl, elm);
}

str
BSKTdump(void *ret)
{
	int bskt;
	BUN cnt;
	BAT *b;
	node *n;
	sql_column *c;
	mvc *m = NULL;
	str msg = MAL_SUCCEED;

	mnstr_printf(GDKout, "#baskets table\n");
	for (bskt = 1; bskt < bsktLimit; bskt++)
		if (baskets[bskt].table_name) {
			msg = getSQLContext(mal_clients, 0, &m, NULL);
			if ( msg != MAL_SUCCEED)
				break;
			cnt = 0;
			n = baskets[bskt].table->columns.set->h;
			c = n->data;
			b = store_funcs.bind_col(m->session->tr,c,RD_INS);
			if( b){
				cnt = BATcount(b);
				BBPunfix(b->batCacheid);
			}

			mnstr_printf(GDKout, "#baskets[%2d] %s.%s columns %d threshold %d window=[%d,%d] time window=[" LLFMT "," LLFMT "] beat " LLFMT " milliseconds" BUNFMT"\n",
					bskt,
					baskets[bskt].schema_name,
					baskets[bskt].table_name,
					baskets[bskt].count,
					baskets[bskt].threshold,
					baskets[bskt].winsize,
					baskets[bskt].winstride,
					baskets[bskt].timeslice,
					baskets[bskt].timestride,
					baskets[bskt].beat,
					cnt);
		}

	(void) ret;
	return msg;
}

str
BSKTappend(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
    int *res = getArgReference_int(stk, pci, 0);
    mvc *m = NULL;
    str msg;
    str sname = *getArgReference_str(stk, pci, 2);
    str tname = *getArgReference_str(stk, pci, 3);
    str cname = *getArgReference_str(stk, pci, 4);
    ptr value = getArgReference(stk, pci, 5);
    int tpe = getArgType(mb, pci, 5);
    sql_schema *s;
    sql_table *t;
    sql_column *c;
    BAT *bn=0, *binsert = 0;

    *res = 0;
    if ((msg = getSQLContext(cntxt, mb, &m, NULL)) != NULL)
        return msg;
    if ((msg = checkSQLContext(cntxt)) != NULL)
        return msg;

    s = mvc_bind_schema(m, sname);
    if (s == NULL)
        throw(SQL, "basket.append", "Schema missing");
    t = mvc_bind_table(m, s, tname);
	if ( t)
		c= mvc_bind_column(m, t, cname);
	else throw(SQL,"basket.append","Stream table %s.%s not accessible for append\n",sname,tname);
	if( c == NULL) 
		throw(SQL,"basket.append","Stream column %s.%s.%s not accessible for append\n",sname,tname,cname);

    if ( isaBatType(tpe) && (binsert = BATdescriptor(*(int *) value)) == NULL)
        throw(SQL, "basket.append", "Cannot access source descriptor");
	if ( ATOMextern(tpe))
		value = *(ptr*) value;

	bn = store_funcs.bind_col(m->session->tr,c,RD_INS);
	if( bn){
		if (binsert)
			BATappend(bn, binsert, TRUE);
		else
			BUNappend(bn, value, TRUE);
		BBPunfix(bn->batCacheid);
	} else throw(SQL, "basket.append", "Cannot access target descriptor");
	if (binsert )
		BBPunfix(((BAT *) binsert)->batCacheid);
	return MAL_SUCCEED;
}

str
BSKTdelete(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
    int *res = getArgReference_int(stk, pci, 0);
    mvc *m = NULL;
    str msg;
    str sname = *getArgReference_str(stk, pci, 2);
    str tname = *getArgReference_str(stk, pci, 3);
    //bat del = getArgReference(stk, pci, 4);
    sql_schema *s;
    sql_table *t;

    *res = 0;
    if ((msg = getSQLContext(cntxt, mb, &m, NULL)) != NULL)
        return msg;
    if ((msg = checkSQLContext(cntxt)) != NULL)
        return msg;

    s = mvc_bind_schema(m, sname);
    if (s == NULL)
        throw(SQL, "basket.delete", "Schema missing");
    t = mvc_bind_table(m, s, tname);
	if (t == NULL)
		 throw(SQL,"basket.delete","Stream table %s.%s not accessible for append\n",sname,tname);
	return MAL_SUCCEED;
}

str
BSKTclear(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
    lng *res = getArgReference_lng(stk, pci, 0);
    mvc *m = NULL;
    str msg;
    str sname = *getArgReference_str(stk, pci, 1);
    str tname = *getArgReference_str(stk, pci, 2);
    sql_schema *s;
    sql_table *t;
	sql_column *c;
	int i, idx;
	BAT *b;

    *res = 0;
    if ((msg = getSQLContext(cntxt, mb, &m, NULL)) != NULL)
        return msg;
    if ((msg = checkSQLContext(cntxt)) != NULL)
        return msg;
    s = mvc_bind_schema(m, sname);
    if (s == NULL)
        throw(SQL, "basket.clear", "Schema missing");
    t = mvc_bind_table(m, s, tname);
	if ( t == NULL)
		throw(SQL,"basket.clear","Stream table %s.%s not accessible for clearing\n",sname,tname);
	idx = BSKTlocate(sname,tname);
	if( idx <= 0)
		throw(SQL,"basket.clear","Stream table %s.%s not registered \n",sname,tname);
	// do actual work
	MT_lock_set(&iotLock);
	for( i=0; baskets[idx].cols[i]; i++){
		c= mvc_bind_column(m, t, baskets[idx].cols[i]);
		if( c){
			b = store_funcs.bind_col(m->session->tr,c,RD_INS);
			if(b){
				BATsetcount(b,0);
				BBPunfix(b->batCacheid);
			}
		}
	}
	MT_lock_unset(&iotLock);
	return MAL_SUCCEED;
}

str
BSKTcommit(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
    str sname = *getArgReference_str(stk, pci, 2);
    str tname = *getArgReference_str(stk, pci, 3);
	int idx;
	(void) cntxt;
	(void) mb;

	idx = BSKTlocate(sname,tname);
	if( idx == 0)
		throw(SQL,"basket.commit","Stream table %s.%s not accessible for commit\n",sname,tname);

	MT_lock_set(&iotLock);
	baskets[idx].count++;
	MT_lock_unset(&iotLock);
	return MAL_SUCCEED;
}

static str
BSKTerrorInternal(bat *ret, str sname, str tname, str err)
{
	int idx;
	idx = BSKTlocate(sname,tname);
	if( idx == 0)
		throw(SQL,"basket.error","Stream table %s.%s not accessible for commit\n",sname,tname);

	if( baskets[idx].errors == NULL)
		baskets[idx].errors = BATnew(TYPE_void, TYPE_str, 0, TRANSIENT);
		
	if( baskets[idx].errors == NULL)
		throw(SQL,"basket.error",MAL_MALLOC_FAIL);

	BUNappend(baskets[idx].errors, err, FALSE);
	
	BBPkeepref(*ret = baskets[idx].errors->batCacheid);
	return MAL_SUCCEED;
}

str
BSKTerror(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	bat *ret  = getArgReference_bat(stk,pci,0);
    str sname = *getArgReference_str(stk, pci, 1);
    str tname = *getArgReference_str(stk, pci, 2);
    str err = *getArgReference_str(stk, pci, 3);
	int idx;
	str msg = MAL_SUCCEED;
	(void) cntxt;
	(void) mb;

	idx = BSKTlocate(sname,tname);
	if( idx == 0)
		throw(SQL,"basket.error","Stream table %s.%s not accessible for commit\n",sname,tname);

	MT_lock_set(&iotLock);
	msg = BSKTerrorInternal(ret,sname,tname,err);
	MT_lock_unset(&iotLock);
	return msg;
}

str
BSKTupdate (Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	(void) cntxt;
	(void) mb;
	(void) stk;
	(void) pci;
	return NULL;
}

/* provide a tabular view for inspection */
str
BSKTtable (Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	bat *schemaId = getArgReference_bat(stk,pci,0);
	bat *nameId = getArgReference_bat(stk,pci,1);
	bat *statusId = getArgReference_bat(stk,pci,2);
	bat *thresholdId = getArgReference_bat(stk,pci,3);
	bat *winsizeId = getArgReference_bat(stk,pci,4);
	bat *winstrideId = getArgReference_bat(stk,pci,5);
	bat *timesliceId = getArgReference_bat(stk,pci,6);
	bat *timestrideId = getArgReference_bat(stk,pci,7);
	bat *beatId = getArgReference_bat(stk,pci,8);
	bat *seenId = getArgReference_bat(stk,pci,9);
	bat *eventsId = getArgReference_bat(stk,pci,10);
	BAT *schema = NULL, *name = NULL, *status = NULL,  *seen = NULL, *events = NULL;
	BAT *threshold = NULL, *winsize = NULL, *winstride = NULL, *beat = NULL;
	BAT *timeslice = NULL, *timestride = NULL;
	int i;
	BAT *bn = NULL;

	(void) mb;

	schema = BATnew(TYPE_void, TYPE_str, BATTINY, TRANSIENT);
	if (schema == 0)
		goto wrapup;
	BATseqbase(schema, 0);
	name = BATnew(TYPE_void, TYPE_str, BATTINY, TRANSIENT);
	if (name == 0)
		goto wrapup;
	BATseqbase(status, 0);
	status = BATnew(TYPE_void, TYPE_str, BATTINY, TRANSIENT);
	if (status == 0)
		goto wrapup;
	BATseqbase(status, 0);
	threshold = BATnew(TYPE_void, TYPE_int, BATTINY, TRANSIENT);
	if (threshold == 0)
		goto wrapup;
	BATseqbase(threshold, 0);
	winsize = BATnew(TYPE_void, TYPE_int, BATTINY, TRANSIENT);
	if (winsize == 0)
		goto wrapup;
	BATseqbase(winsize, 0);
	winstride = BATnew(TYPE_void, TYPE_int, BATTINY, TRANSIENT);
	if (winstride == 0)
		goto wrapup;
	BATseqbase(winstride, 0);
	timeslice = BATnew(TYPE_void, TYPE_int, BATTINY, TRANSIENT);
	if (timeslice == 0)
		goto wrapup;
	BATseqbase(timeslice, 0);
	timestride = BATnew(TYPE_void, TYPE_int, BATTINY, TRANSIENT);
	if (timestride == 0)
		goto wrapup;
	BATseqbase(timestride, 0);
	beat = BATnew(TYPE_void, TYPE_int, BATTINY, TRANSIENT);
	if (beat == 0)
		goto wrapup;
	BATseqbase(beat, 0);
	seen = BATnew(TYPE_void, TYPE_timestamp, BATTINY, TRANSIENT);
	if (seen == 0)
		goto wrapup;
	BATseqbase(seen, 0);
	events = BATnew(TYPE_void, TYPE_int, BATTINY, TRANSIENT);
	if (events == 0)
		goto wrapup;
	BATseqbase(events, 0);


	for (i = 1; i < bsktTop; i++)
		if (baskets[i].table_name) {
			BUNappend(schema, baskets[i].schema_name, FALSE);
			BUNappend(name, baskets[i].table_name, FALSE);
			BUNappend(status, statusname[baskets[i].status], FALSE);
			BUNappend(threshold, &baskets[i].threshold, FALSE);
			BUNappend(winsize, &baskets[i].winsize, FALSE);
			BUNappend(winstride, &baskets[i].winstride, FALSE);
			BUNappend(timeslice, &baskets[i].timeslice, FALSE);
			BUNappend(timestride, &baskets[i].timestride, FALSE);
			BUNappend(beat, &baskets[i].beat, FALSE);
			BUNappend(seen, &baskets[i].seen, FALSE);
			bn = BSKTbindColumn(cntxt,baskets[i].schema_name, baskets[i].table_name, baskets[i].cols[0]);
			baskets[i].events = bn ? (int) BATcount( bn): 0;
			BUNappend(events, &baskets[i].events, FALSE);
		}

	BBPkeepref(*schemaId = schema->batCacheid);
	BBPkeepref(*nameId = name->batCacheid);
	BBPkeepref(*statusId = status->batCacheid);
	BBPkeepref(*thresholdId = threshold->batCacheid);
	BBPkeepref(*winsizeId = winsize->batCacheid);
	BBPkeepref(*winstrideId = winstride->batCacheid);
	BBPkeepref(*timesliceId = timeslice->batCacheid);
	BBPkeepref(*timestrideId = timestride->batCacheid);
	BBPkeepref(*beatId = beat->batCacheid);
	BBPkeepref(*seenId = seen->batCacheid);
	BBPkeepref(*eventsId = events->batCacheid);
	return MAL_SUCCEED;
wrapup:
	if (schema)
		BBPunfix(schema->batCacheid);
	if (name)
		BBPunfix(name->batCacheid);
	if (status)
		BBPunfix(status->batCacheid);
	if (threshold)
		BBPunfix(threshold->batCacheid);
	if (winsize)
		BBPunfix(winsize->batCacheid);
	if (winstride)
		BBPunfix(winstride->batCacheid);
	if (timeslice)
		BBPunfix(timeslice->batCacheid);
	if (timestride)
		BBPunfix(timestride->batCacheid);
	if (beat)
		BBPunfix(beat->batCacheid);
	if (seen)
		BBPunfix(seen->batCacheid);
	if (events)
		BBPunfix(events->batCacheid);
	throw(SQL, "iot.baskets", MAL_MALLOC_FAIL);
}

str
BSKTtableerrors(bat *nameId, bat *errorId)
{
	BAT  *name, *error;
	BATiter bi;
	BUN p, q;
	int i;
	name = BATnew(TYPE_void, TYPE_str, BATTINY, PERSISTENT);
	if (name == 0)
		throw(SQL, "baskets.errors", MAL_MALLOC_FAIL);
	error = BATnew(TYPE_void, TYPE_str, BATTINY, TRANSIENT);
	if (error == 0) {
		BBPunfix(name->batCacheid);
		throw(SQL, "baskets.errors", MAL_MALLOC_FAIL);
	}

	for (i = 1; i < bsktTop; i++)
		if (BATcount(baskets[i].errors) > 0) {
			bi = bat_iterator(baskets[i].errors);
			BATloop(baskets[i].errors, p, q)
			{
				str err = BUNtail(bi, p);
				BUNappend(name, &baskets[i].table_name, FALSE);
				BUNappend(error, err, FALSE);
			}
		}


	BBPkeepref(*nameId = name->batCacheid);
	BBPkeepref(*errorId = error->batCacheid);
	return MAL_SUCCEED;
}
