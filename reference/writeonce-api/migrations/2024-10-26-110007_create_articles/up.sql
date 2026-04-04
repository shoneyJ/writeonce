-- Your SQL goes here
CREATE TABLE articles (
  id SERIAL PRIMARY KEY,
  title VARCHAR NOT NULL,
  sys_title VARCHAR NOT NULL,
  published BOOLEAN NOT NULL DEFAULT FALSE
)