# Update read-only tunable
SELECT name, read_only FROM comdb2_tunables WHERE name = 'dir';
PUT TUNABLE dir '/tmp';
SELECT value AS 'nowatch' FROM comdb2_tunables WHERE name = 'nowatch';
PUT TUNABLE nowatch 1;
SELECT value AS 'nowatch' FROM comdb2_tunables WHERE name = 'nowatch';

# Invalid tunable
PUT TUNABLE nonexistent 100;

# Invalid tunable value
SELECT value AS 'allow_broken_datetimes' FROM comdb2_tunables WHERE name = 'allow_broken_datetimes';
PUT TUNABLE allow_broken_datetimes;
SELECT value AS 'allow_broken_datetimes' FROM comdb2_tunables WHERE name = 'allow_broken_datetimes';
PUT TUNABLE allow_broken_datetimes 100;
SELECT value AS 'allow_broken_datetimes' FROM comdb2_tunables WHERE name = 'allow_broken_datetimes';
PUT TUNABLE allow_broken_datetimes 'onn';
SELECT value AS 'allow_broken_datetimes' FROM comdb2_tunables WHERE name = 'allow_broken_datetimes';
PUT TUNABLE allow_broken_datetimes 'of';

SELECT name, value, read_only FROM comdb2_tunables WHERE name = 'latch_max_poll';

PUT TUNABLE latch_max_poll 100;
SELECT value FROM comdb2_tunables WHERE name = 'latch_max_poll';

PUT TUNABLE latch_max_poll 'xx';
PUT TUNABLE latch_max_poll xx;
SELECT value FROM comdb2_tunables WHERE name = 'latch_max_poll';

PUT TUNABLE latch_max_poll '10';
SELECT value FROM comdb2_tunables WHERE name = 'latch_max_poll';

# Test dynamic tunable using 'exec procedure' & 'put tunable'.
SELECT value AS 'debug_rowlocks' FROM comdb2_tunables WHERE name = 'debug_rowlocks';
PUT TUNABLE debug_rowlocks 0;
SELECT value AS 'debug_rowlocks' FROM comdb2_tunables WHERE name = 'debug_rowlocks';
exec procedure sys.cmd.send('debug_rowlocks')
SELECT value AS 'debug_rowlocks' FROM comdb2_tunables WHERE name = 'debug_rowlocks';
exec procedure sys.cmd.send('nodebug_rowlocks')
SELECT value AS 'debug_rowlocks' FROM comdb2_tunables WHERE name = 'debug_rowlocks';
SELECT value AS 'nodebug_rowlocks' FROM comdb2_tunables WHERE name = 'nodebug_rowlocks';

# Test composite tunables.
SELECT name AS 'logmsg tunables' FROM comdb2_tunables WHERE name LIKE 'logmsg%';
SELECT value AS 'logmsg.level' FROM comdb2_tunables WHERE name = 'logmsg.level';
PUT TUNABLE 'logmsg.level' 'xxx';
PUT TUNABLE 'logmsg.level' 'error';
SELECT value AS 'logmsg.level' FROM comdb2_tunables WHERE name = 'logmsg.level';
exec procedure sys.cmd.send('logmsg level xxx');
exec procedure sys.cmd.send('logmsg level debug');
SELECT value AS 'logmsg.level' FROM comdb2_tunables WHERE name = 'logmsg.level';

SELECT name AS 'appsockpool tunables' FROM comdb2_tunables WHERE name LIKE 'appsockpool%';
SELECT value AS 'appsockpool.maxt' FROM comdb2_tunables WHERE name = 'appsockpool.maxt';
PUT TUNABLE 'appsockpool.maxt' 'xxx';
PUT TUNABLE 'appsockpool.maxt' 101;
SELECT value AS 'appsockpool.maxt' FROM comdb2_tunables WHERE name = 'appsockpool.maxt';
exec procedure sys.cmd.send('appsockpool maxt xxx');
exec procedure sys.cmd.send('appsockpool maxt 102');
SELECT value AS 'appsockpool.maxt' FROM comdb2_tunables WHERE name = 'appsockpool.maxt';
