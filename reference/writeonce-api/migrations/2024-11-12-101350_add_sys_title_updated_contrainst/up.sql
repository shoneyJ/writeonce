-- Your SQL goes here
ALTER TABLE articles DROP CONSTRAINT articles_pkey;

ALTER TABLE articles
ADD CONSTRAINT articles_pkey PRIMARY KEY (sys_title);