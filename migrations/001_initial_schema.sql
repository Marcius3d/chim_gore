-- CHIM Gore: plugin-owned settings and counters (key/value).
CREATE SCHEMA IF NOT EXISTS plugins;

CREATE TABLE IF NOT EXISTS plugins.chim_gore_settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL DEFAULT '',
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);
