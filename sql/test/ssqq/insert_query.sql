select * from quser;

select * from query;

select * from ssqq_queue;

declare id_result int;

set id_result = -1;

set id_result = insert_query(NULL, 'select * from example', 0, NULL);

select id_result;

select * from query;

set id_result = -1;

set id_result = insert_query(1, 'select * from example', 0, NULL);

select id_result;

select * from query;

set id_result = -1;

set id_result = insert_query(1, NULL, 0, NULL);

select id_result;

select * from query;

set id_result = -1;

set id_result = insert_query(NULL, 'select * from example', 0, NULL);

select id_result;

select * from quser;

select * from query;

select * from ssqq_queue;
