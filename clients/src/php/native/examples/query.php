<?php

/**
* Connect to a database and perform a query.
*/

require '../lib/php_monetdb.php';

define("DB", "php_demo");

/* Establish a connection and report errors in case they occour */
$db = monetdb_connect($host = "127.0.0.1", $port = "50000", $database = DB , $username = "monetdb", $password = "monetdb" ) or trigger_error(monetdb_last_error());

/* Fire a query */

$query = "SELECT * FROM TABLES";
$res = monetdb_query($db, monetdb_escape_string($query)) or trigger_error(monetdb_last_error());

/* Print the number of rows in the result set */
print "Rows: " . monetdb_num_rows($res) . "\n";

/* Iterate over the result set returning rows as objects */
while ( $row = monetdb_fetch_object($res) )
{
	print_r($row);
}

/* Free the result set */
monetdb_free_result($res);

/* Disconnect from the database */
if (monetdb_connected($db)) {
	monetdb_disconnect($db);
}

?>