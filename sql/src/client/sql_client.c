
#include <stdlib.h>
#include <string.h>
#include "mem.h"
#include "sqlexecute.h"
#include <comm.h>
#include <sys/stat.h>
#include <catalog.h>

#ifdef HAVE_LIBGETOPT 
#include <getopt.h>
#endif

stream *ws = NULL, *rs = NULL;

/*
 * Debug levels
 * 	0 	no debugging
 * 	1 	not used
 * 	2	output additional time statements
 * 	4	not used
 * 	8	output code to stderr
 * 	16	output parsed SQL
 * 	32	execute but no output write to the client
 * 	64 	output code only, no excution on the server.
 *     128 	export code in xml.
 */ 
extern catalog *catalog_create_stream( stream *s, context *lc );

void usage( char *prog ){
	fprintf(stderr, "sql_client\n");
	fprintf(stderr, "\toptions:\n");
	fprintf(stderr, "\t\t -d          | --debug=[level]\n"); 
	fprintf(stderr, "\t\t -h hostname | --host=hostname  /* host to connect to */\n");
	fprintf(stderr, "\t\t -p portnr   | --port=portnr    /* port to connect to */\n");
	fprintf(stderr, "\t\t -s schema   | --schema=schema  /* schema to use */\n");
	fprintf(stderr, "\t\t -u user     | --user=user      /* user id */\n" );
	fprintf(stderr, "\t\t -o level    | --optimize=level /* optimzation level */\n" );
	exit(-1);
}

void receive( stream *rs ){
	int flag = 0;
	if (rs->readInt(rs, &flag) && flag != COMM_DONE){
		char buf[BLOCK+1], *n = buf;
		int last = 0;
		int nr = bs_read_next(rs,buf,&last);
		int nRows = strtol(n,&n,10);

		n++; /* skip newline */
		fwrite( n, nr - (n - buf), 1, stdout );
		while(!last){
			int nr = bs_read_next(rs,buf,&last);
			fwrite( buf, nr, 1, stdout );
		}
		if (nRows > 1)
			printf("%d Rows affected\n", nRows );
		else if (nRows == 1)
			printf("1 Row affected\n" );
		else 
			printf("no Rows affected\n" );
	} else if (flag != COMM_DONE){
		printf("flag %d\n", flag);
	}
}


void clientAccept( context *lc, stream *rs ){
	int err = 0;
	stream *in = file_rastream( stdin, "<stdin>" );
	stmt *s = NULL;
	char eot[1];

	while(lc->cur != EOF ){
		s = sqlnext(lc, in, &err);
		if (err) break;
		if (s){
	    		int nr = 1;
			if (lc->debug&128){
	    			stmt2xml( s, &nr, lc );
				stmt_reset( s );
				nr= 1;
			} 
	    		stmt_dump( s, &nr, lc );

	    		lc->out->flush( lc->out );
		}
		if (!(lc->debug&64) && s && s->type == st_output){
			receive(rs);
		}
		if (s) stmt_destroy(s);
	}
	in->destroy(in);

	eot[0] = EOT;
	ws->write( ws, eot, 1, 1 );
	ws->flush( ws );
	receive(rs);
}

/*
When using alloca(3) in a shared library, Intel's "C++ Compiler for 32-bit
applications, Version 5.0.1 Beta Build 010528D0" seems to require another
reference to alloca(3) in the file that contains the main(), otherwise it
complains about an "undefined reference to `\_alloca\_probe' when linking
the shared library to the executable.
Hence, we define this FAKE\_ALLOCA\_CALL and call it in the respective
main()s just before the final return.
*/
#if ( defined(__INTEL_COMPILER) && (!defined(STATIC)) )
# include <alloca.h>
# define FAKE_ALLOCA_CALL (void)alloca(0);
#else
# define FAKE_ALLOCA_CALL ;
#endif

int
main(int ac, char **av)
{
	char buf[BUFSIZ];
	char *schema = NULL;
	char *user = NULL, *passwd = "";
	char *prog = *av, *host = "localhost";
	int debug = 0, fd = 0, port = 45123;
	int opt = SQL_FAST_INSERT, i = 0;
	context lc;

	static struct option long_options[] =
             {
               {"debug", 2, 0, 'd'},
               {"host", 1, 0, 'h'},
               {"port", 1, 0, 'p'},
               {"passwd", 1, 0, 'P'},
               {"schema", 1, 0, 's'},
               {"user", 1, 0, 'u'},
               {"optimize", 1, 0, 'o'},
               {0, 0, 0, 0}
             };

	while(1){
		int option_index = 0;

		int c = getopt_long( ac, av, "dh:p:s:u:o:", 
				long_options, &option_index);

		if (c == -1)
			break;

		switch (c){
		case 0:
			/* all long options are mapped on their short version */
			printf("option %s", long_options[option_index].name);
			if (optarg)
				printf( " with arg %s", optarg );
			printf("\n");
			usage(prog);
			break;
		case 'd':
			debug=2;
			if (optarg) debug=strtol(optarg,NULL,10);
			break;
		case 'h':
			host=_strdup(optarg);
			break;
		case 'p':
			port=strtol(optarg,NULL,10);
			break;
		case 'P':
			passwd=strdup(optarg);
			break;
		case 's':
			schema=_strdup(optarg);
			break;
		case 'u':
			user=_strdup(optarg);
			break;
		case 'o':
			opt=strtol(optarg,NULL,10);
			break;
		case '?':
			usage(prog);
		default:
			printf( "?? getopt returned character code 0%o ??\n",c);
			usage(prog);
		}
	}
	if (optind < ac){
		printf("some arguments are not parsed by getopt\n");
		while(optind < ac)
			printf("%s ", av[optind++]);
		printf("\n");
		usage(prog);
	}

	fd = client( host, port);
	rs = block_stream(socket_rstream( fd, "sql client read"));
	ws = block_stream(socket_wstream( fd, "sql client write"));
	if (rs->errnr || ws->errnr){
		fprintf(stderr, "sockets not opened correctly\n");
		exit(1);
	}
	if (!schema) schema = _strdup("default-schema");
	if (!user) user = _strdup("sqladmin");

	i = snprintf(buf, BUFSIZ, "info(\"%s\", %d, %d);\n", user, debug, opt);
	ws->write( ws, buf, i, 1 );
	ws->flush( ws );

	i = snprintf(buf, BUFSIZ, "milsql();\n" );
	ws->write( ws, buf, i, 1 );
	ws->flush( ws );

	/* following part needs a rework
	 * first we need a connection (aka login(user,passwd));) (server should
	 * wait for this command right after the api command )
	 *
	 * Then the mvc is created on the server side, and the client sends
	 * a mvc_database command (selecting the apropriete db)
	 *
	 * */
	i = snprintf(buf, BUFSIZ, "myc := mvc_create(%d);\n", debug );
	ws->write( ws, buf, i, 1 );
	ws->flush( ws );
	if (debug&64) fprintf(stdout, buf );

	i = snprintf(buf, BUFSIZ, "mvc_login(myc, \"%s\",\"%s\",\"%s\");\n", 
				schema, user, passwd );
	ws->write( ws, buf, i, 1 );
	ws->flush( ws );
	if (debug&64) fprintf(stdout, buf );

	memset(&lc, 0, sizeof(lc));
	sql_init_context( &lc, ws, debug, default_catalog_create() );
	lc.optimize = opt;
	catalog_create_stream( rs, &lc );

	lc.cat->cc_getschema( lc.cat, schema, user );
	lc.cur = ' ';
	if (debug&64){
		ws = lc.out;
		lc.out = file_wastream(stdout, "<stdout>" );
	}
	clientAccept( &lc, rs );

	if (rs){
	       	rs->close(rs);
	       	rs->destroy(rs);
	}
	if (debug&64){
	       	lc.out->close(lc.out);
	       	lc.out->destroy(lc.out);
		lc.out = ws;
	}
	sql_exit_context( &lc );
	FAKE_ALLOCA_CALL; /* buggy Intel C/C++ compiler for Linux ... */
	return 0;
} /* main */

