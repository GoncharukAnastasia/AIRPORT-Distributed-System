-- Таблица машин катеринга (аналогично followme_vehicles)
CREATE TABLE IF NOT EXISTS catering_trucks (
    vehicle_id   TEXT PRIMARY KEY,
    current_node TEXT NOT NULL DEFAULT 'FS-1',
    status       TEXT NOT NULL DEFAULT 'empty',
    flight_id    TEXT NULL,
    mission_type TEXT NULL,
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT chk_catering_status CHECK (
        status IN ('empty', 'reserved', 'moveToHub', 'moveToPlane', 'servicing', 'returning')
    )
);

CREATE INDEX IF NOT EXISTS idx_catering_status ON catering_trucks(status);
