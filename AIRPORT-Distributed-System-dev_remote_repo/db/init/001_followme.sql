CREATE TABLE IF NOT EXISTS followme_vehicles (
                                                 vehicle_id   TEXT PRIMARY KEY,
                                                 current_node TEXT NOT NULL DEFAULT 'FS-1',
                                                 status       TEXT NOT NULL DEFAULT 'empty',
                                                 flight_id    TEXT NULL,
                                                 updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT chk_followme_status CHECK (
                                             status IN ('empty', 'reserved', 'moveToLandingPosition', 'movingWithPlane', 'returning')
    )
    );

CREATE INDEX IF NOT EXISTS idx_followme_status ON followme_vehicles(status);

-- Стартовые машинки (можно убрать, если хотите наполнять только через API /v1/vehicles/init)
INSERT INTO followme_vehicles (vehicle_id, current_node, status, flight_id)
VALUES
    ('FM-1', 'FS-1', 'empty', NULL),
    ('FM-2', 'FS-1', 'empty', NULL)
    ON CONFLICT (vehicle_id) DO NOTHING;