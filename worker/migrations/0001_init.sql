-- The whole board: one row per live room. No index besides the keys - every
-- index costs an extra written row on the free plan, and the table stays small.
CREATE TABLE rooms (
  room_id       TEXT PRIMARY KEY,
  code          TEXT NOT NULL UNIQUE,
  token_hash    TEXT NOT NULL,          -- SHA-256 of the host token
  addr_hash     TEXT NOT NULL,          -- keyed hash of the creator's address (limits)
  address       TEXT NOT NULL,          -- what joiners connect to: detected IP or one the host gave
  port          INTEGER NOT NULL,
  name          TEXT NOT NULL,
  mod_id        TEXT NOT NULL,
  mod_version   TEXT NOT NULL,
  difficulty    TEXT NOT NULL,
  mode          TEXT NOT NULL,
  act           INTEGER,
  max_players   INTEGER NOT NULL,
  public        INTEGER NOT NULL,
  password_hash TEXT,                   -- PBKDF2, salt:hash, NULL when open
  data          TEXT NOT NULL,          -- JSON: host_player, roster, vr, joiners
  created_at    INTEGER NOT NULL,       -- unix seconds
  last_seen     INTEGER NOT NULL
);

-- Room creations and wrong passwords, for the rate limits.
CREATE TABLE attempts (
  key TEXT NOT NULL,
  at  INTEGER NOT NULL
);
