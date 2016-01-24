CREATE TABLE flights (
    ID      INTEGER PRIMARY KEY AUTOINCREMENT
                    NOT NULL,
    frameID INT,
    sondeID STRING,
    weekday STRING,
    clock   TIME,
    lat     DOUBLE,
    lon     DOUBLE,
    alt     DOUBLE,
    freq    STRING,
    misc    STRING
);

CREATE VIEW vw_sort_sondes AS
    SELECT *
      FROM flights
     GROUP BY sondeid,
              frameid,
              clock
     ORDER BY sondeid,
              frameid;

