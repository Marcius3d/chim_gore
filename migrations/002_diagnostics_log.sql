-- CHIM Gore 0.2: diagnostics log shown on the plugin page and in the diagnostic file.
CREATE TABLE IF NOT EXISTS plugins.chim_gore_log (
    id BIGSERIAL PRIMARY KEY,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    source TEXT NOT NULL DEFAULT 'server',
    level TEXT NOT NULL DEFAULT 'info',
    message TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_chim_gore_log_source ON plugins.chim_gore_log (source, id);
