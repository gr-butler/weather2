-- =============================================================================
--  Long-term weather record schema for the weather2 ESP32 station.
--
--  This replaces the minimal `weather` table the original Go app wrote to
--  (record_date, temperature, pressure, rain_mm, wind_speed, wind_gust,
--  wind_direction) with a richer, self-documenting table designed to be the
--  permanent historical record. It is populated by Telegraf, which scrapes the
--  device's Prometheus /metrics endpoint every 15 minutes
--  (see telegraf-weather.conf).
--
--  Apply with:
--    psql -h 192.168.3.10 -U weather -d weather -f weather_schema.sql
--
--  Column names double as the Telegraf field names (see the rename processor in
--  telegraf-weather.conf) — keep the two files in sync.
-- =============================================================================

CREATE TABLE IF NOT EXISTS weather_observations (
    -- Observation time (scrape time). PRIMARY KEY so re-runs can't duplicate a
    -- reading and so the table is TimescaleDB-hypertable ready (the partition
    -- column must be part of every unique index).
    time                TIMESTAMPTZ      NOT NULL PRIMARY KEY,

    -- Atmospheric (BME280 / MCP9808).
    temperature_c       DOUBLE PRECISION,   -- outdoor temperature, degrees C
    humidity_pct        DOUBLE PRECISION,   -- relative humidity, 0-100 %
    pressure_hpa        DOUBLE PRECISION,   -- barometric pressure, hPa

    -- Rain (tipping bucket).
    rain_rate_mm_min    DOUBLE PRECISION,   -- rain rate over last 1 min, mm/min
    rain_day_mm         DOUBLE PRECISION,   -- daily total (09:00-09:00), mm

    -- Wind (masthead node over CAN; averaged on the receiver).
    wind_speed_mph      DOUBLE PRECISION,   -- 60 s rolling average, mph
    wind_gust_mph       DOUBLE PRECISION,   -- max 3 s average (10 min), mph
    wind_direction_deg  DOUBLE PRECISION,   -- 0-360 degrees

    -- Environment Agency river level (context, not a weather sensor).
    river_level_m       DOUBLE PRECISION    -- river stage, metres
);

-- Fast "most recent / time-range" queries for dashboards.
CREATE INDEX IF NOT EXISTS idx_weather_observations_time
    ON weather_observations (time DESC);

-- Self-documenting units, so the record stays understandable years from now.
COMMENT ON TABLE  weather_observations                     IS 'Long-term weather record, one row per 15-min scrape of the weather2 station.';
COMMENT ON COLUMN weather_observations.time                IS 'Observation timestamp (UTC).';
COMMENT ON COLUMN weather_observations.temperature_c       IS 'Outdoor temperature (degrees Celsius).';
COMMENT ON COLUMN weather_observations.humidity_pct        IS 'Relative humidity (0-100 %).';
COMMENT ON COLUMN weather_observations.pressure_hpa        IS 'Barometric pressure (hPa).';
COMMENT ON COLUMN weather_observations.rain_rate_mm_min    IS 'Rain rate over the last minute (mm/min).';
COMMENT ON COLUMN weather_observations.rain_day_mm         IS 'Daily rainfall total, 09:00-09:00 local (mm).';
COMMENT ON COLUMN weather_observations.wind_speed_mph      IS '60-second rolling-average wind speed (mph).';
COMMENT ON COLUMN weather_observations.wind_gust_mph       IS 'Maximum 3-second average wind speed over 10 min (mph).';
COMMENT ON COLUMN weather_observations.wind_direction_deg  IS 'Wind direction (0-360 degrees).';
COMMENT ON COLUMN weather_observations.river_level_m       IS 'Environment Agency river level (metres).';

-- Ensure the Telegraf 'weather' role owns the table so it can INSERT rows (and
-- add columns if a new field ever appears). Without this, a table created by
-- the postgres superuser is not writable by the weather login role. Skipped if
-- the role does not exist yet.
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'weather') THEN
    EXECUTE 'ALTER TABLE weather_observations OWNER TO weather';
  END IF;
END $$;

-- ---------------------------------------------------------------------------
-- OPTIONAL: TimescaleDB
-- If the TimescaleDB extension is installed, convert the table to a hypertable
-- for efficient long-term storage (native compression, retention policies and
-- continuous aggregates for daily/monthly summaries). Run once, after creation:
--
--   CREATE EXTENSION IF NOT EXISTS timescaledb;
--   SELECT create_hypertable('weather_observations', by_range('time'),
--                            if_not_exists => TRUE);
-- ---------------------------------------------------------------------------

-- ---------------------------------------------------------------------------
-- Backfill
-- The original Go app's `weather` table (its Postgres history) was LOST during
-- the server's Ubuntu 24.04 / PostgreSQL 16 upgrade — the old cluster was not
-- migrated and no logical DB dump exists, so there is nothing to import from
-- Postgres.
--
-- The surviving long-term record lives in Prometheus (~2.2 years at 15 s
-- resolution). Seed this table from it with the companion script:
--
--   prometheus_backfill.py   ->   weather_history.csv   ->   \copy (see below)
--
-- Load procedure (idempotent — safe to re-run, won't clash with Telegraf rows):
--   CREATE TEMP TABLE _wo_stage (LIKE weather_observations INCLUDING DEFAULTS);
--   \copy _wo_stage (time,temperature_c,humidity_pct,pressure_hpa,rain_rate_mm_min,rain_day_mm,wind_speed_mph,wind_gust_mph,wind_direction_deg,river_level_m) FROM 'weather_history.csv' WITH (FORMAT csv, HEADER true)
--   INSERT INTO weather_observations SELECT * FROM _wo_stage ON CONFLICT (time) DO NOTHING;
-- ---------------------------------------------------------------------------
