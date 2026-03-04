CREATE TABLE IF NOT EXISTS refueler_vehicles (
                                                 vehicle_id    TEXT PRIMARY KEY,
                                                 current_node  TEXT NOT NULL DEFAULT 'RS-1',
                                                 status        TEXT NOT NULL DEFAULT 'empty',
                                                 flight_id     TEXT NULL,
                                                 parking_node  TEXT NULL,
                                                 updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT chk_refueler_status CHECK (
                                             status IN ('empty', 'reserved', 'moveToRefuelPosition', 'refueling', 'returning')
    )
    );

CREATE INDEX IF NOT EXISTS idx_refueler_status ON refueler_vehicles(status);

INSERT INTO refueler_vehicles (vehicle_id, current_node, status, flight_id, parking_node)
VALUES
    ('RF-1', 'RS-1', 'empty', NULL, NULL),
    ('RF-2', 'RS-1', 'empty', NULL, NULL)
    ON CONFLICT (vehicle_id) DO NOTHING;