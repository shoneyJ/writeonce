-- This file should undo anything in `up.sql`
ALTER TABLE public.articles
DROP COLUMN do_aws_sync;