-- This file should undo anything in `up.sql`
ALTER TABLE articles DROP CONSTRAINT articles_pkey;

ALTER TABLE articles
ADD CONSTRAINT articles_pkey PRIMARY KEY (id);