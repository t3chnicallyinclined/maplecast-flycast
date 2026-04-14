-- ============================================================================
-- MAPLECAST HUB — SurrealDB Schema
--
-- Namespace: maplecast, Database: hub
-- Tables: operator, node
--
-- Run: surreal import --conn http://localhost:8000 \
--        --ns maplecast --db hub hub/src/schema.surql
-- ============================================================================

-- Approved operators who can register game server nodes
DEFINE TABLE operator SCHEMAFULL;
DEFINE FIELD name       ON operator TYPE string;
DEFINE FIELD token_hash ON operator TYPE string;
DEFINE FIELD approved   ON operator TYPE bool DEFAULT true;
DEFINE FIELD max_nodes  ON operator TYPE int DEFAULT 5;
DEFINE FIELD created_at ON operator TYPE datetime DEFAULT time::now();
DEFINE INDEX idx_operator_name ON operator FIELDS name UNIQUE;

-- Registered game server nodes in the distributed network
DEFINE TABLE node SCHEMAFULL;
DEFINE FIELD node_id      ON node TYPE string;
DEFINE FIELD operator     ON node TYPE string;
DEFINE FIELD name         ON node TYPE string;
DEFINE FIELD region       ON node TYPE string;
DEFINE FIELD public_host  ON node TYPE string;
DEFINE FIELD ports        ON node TYPE object;
DEFINE FIELD tls          ON node TYPE bool DEFAULT false;
DEFINE FIELD capacity     ON node TYPE object;
DEFINE FIELD rom_hash     ON node TYPE string;
DEFINE FIELD version      ON node TYPE string;
DEFINE FIELD status       ON node TYPE string DEFAULT "ready";
DEFINE FIELD geo          ON node TYPE option<object>;
DEFINE FIELD metrics      ON node TYPE option<object>;
DEFINE FIELD stats        ON node TYPE object;
DEFINE FIELD last_heartbeat ON node TYPE datetime;
DEFINE FIELD registered_at  ON node TYPE datetime DEFAULT time::now();
DEFINE FIELD stale_count    ON node TYPE int DEFAULT 0;
DEFINE INDEX idx_node_id ON node FIELDS node_id UNIQUE;
DEFINE INDEX idx_node_status ON node FIELDS status;
