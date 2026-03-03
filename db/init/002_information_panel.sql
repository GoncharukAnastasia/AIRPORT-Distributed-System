CREATE TABLE IF NOT EXISTS info_flights (
                                            flight_id     TEXT PRIMARY KEY,
                                            scheduled_at  BIGINT NOT NULL,  -- epoch seconds
                                            status        TEXT NOT NULL,
                                            phase         TEXT NOT NULL,
                                            parking_node  TEXT NULL,
                                            updated_at    BIGINT NOT NULL,  -- epoch seconds
                                            CONSTRAINT chk_info_flights_phase CHECK (phase IN ('airborne', 'grounded', 'terminal'))
    );

CREATE INDEX IF NOT EXISTS idx_info_flights_status ON info_flights(status);
CREATE INDEX IF NOT EXISTS idx_info_flights_phase ON info_flights(phase);
CREATE INDEX IF NOT EXISTS idx_info_flights_scheduled_at ON info_flights(scheduled_at);

-- Стартовые данные (аналог старого /v1/flights/init)
INSERT INTO info_flights (flight_id, scheduled_at, status, phase, parking_node, updated_at)
VALUES
    ('SU100', EXTRACT(EPOCH FROM NOW())::bigint, 'Scheduled', 'airborne', NULL, EXTRACT(EPOCH FROM NOW())::bigint),
    ('SU200', EXTRACT(EPOCH FROM NOW())::bigint, 'Parked',    'grounded', 'P-3', EXTRACT(EPOCH FROM NOW())::bigint)
    ON CONFLICT (flight_id) DO NOTHING;