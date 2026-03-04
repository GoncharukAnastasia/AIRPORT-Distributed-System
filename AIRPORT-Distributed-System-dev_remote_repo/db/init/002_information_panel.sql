CREATE TABLE IF NOT EXISTS info_flights (
                                            flight_id      TEXT PRIMARY KEY,
                                            plane_id       TEXT NOT NULL DEFAULT '',
                                            flight_type    TEXT NOT NULL DEFAULT 'depart',
                                            from_city      TEXT NOT NULL DEFAULT '',
                                            to_city        TEXT NOT NULL DEFAULT '',
                                            scheduled_at   BIGINT NOT NULL,  -- epoch seconds
                                            scheduled_time TEXT NOT NULL DEFAULT '',
                                            arrival_time   TEXT NOT NULL DEFAULT '',
                                            status         TEXT NOT NULL,
                                            gate           TEXT NOT NULL DEFAULT '',
                                            plane_parking  TEXT NOT NULL DEFAULT '',
                                            runway         TEXT NOT NULL DEFAULT '',
                                            phase          TEXT NOT NULL,
                                            parking_node   TEXT NULL,
                                            auto_manage    BOOLEAN NOT NULL DEFAULT FALSE,
                                            updated_at     BIGINT NOT NULL,  -- epoch seconds
                                            CONSTRAINT chk_info_flights_phase CHECK (phase IN ('airborne', 'grounded', 'terminal')),
                                            CONSTRAINT chk_info_flights_type CHECK (flight_type IN ('arrive', 'depart'))
    );

CREATE INDEX IF NOT EXISTS idx_info_flights_status ON info_flights(status);
CREATE INDEX IF NOT EXISTS idx_info_flights_phase ON info_flights(phase);
CREATE INDEX IF NOT EXISTS idx_info_flights_type ON info_flights(flight_type);
CREATE INDEX IF NOT EXISTS idx_info_flights_scheduled_at ON info_flights(scheduled_at);

-- Стартовые данные (аналог старого /v1/flights/init)
INSERT INTO info_flights (
    flight_id,
    plane_id,
    flight_type,
    from_city,
    to_city,
    scheduled_at,
    scheduled_time,
    arrival_time,
    status,
    gate,
    plane_parking,
    runway,
    phase,
    parking_node,
    auto_manage,
    updated_at
)
VALUES
    (
        'SU100',
        'PL-100',
        'arrive',
        'Kazan',
        'Moscow',
        EXTRACT(EPOCH FROM NOW())::bigint,
        to_char(now(), 'YYYY-MM-DD"T"HH24:MI:SS"Z"'),
        '',
        'Scheduled',
        '',
        '',
        '',
        'airborne',
        NULL,
        FALSE,
        EXTRACT(EPOCH FROM NOW())::bigint
    ),
    (
        'SU200',
        'PL-200',
        'depart',
        'Moscow',
        'Saint-Petersburg',
        EXTRACT(EPOCH FROM NOW())::bigint,
        to_char(now(), 'YYYY-MM-DD"T"HH24:MI:SS"Z"'),
        '',
        'Parked',
        'G3',
        'PR3',
        '',
        'grounded',
        'P-3',
        FALSE,
        EXTRACT(EPOCH FROM NOW())::bigint
    )
    ON CONFLICT (flight_id) DO NOTHING;
