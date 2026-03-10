CREATE TABLE reporting_events (

    id SERIAL PRIMARY KEY,
    event_id TEXT UNIQUE,
    event_type TEXT,
    flight_id TEXT,
    passenger_id TEXT,
    count INT,
    timestamp TIMESTAMP
);


CREATE TABLE flight_statistics (

    flight_id TEXT PRIMARY KEY,

    tickets_sold INT DEFAULT 0,
    tickets_refunded INT DEFAULT 0,

    passengers_checked_in INT DEFAULT 0,
    passengers_boarded INT DEFAULT 0
);