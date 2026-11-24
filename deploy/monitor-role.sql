-- The metrics exporter's role (ADR 0003): statistics, but no job tables.
\set password `cat /run/vrp/monitor/password`
SELECT 'CREATE ROLE vrp_monitor LOGIN'
WHERE NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'vrp_monitor')\gexec
ALTER ROLE vrp_monitor PASSWORD :'password';
GRANT pg_monitor TO vrp_monitor;
