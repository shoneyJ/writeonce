// @generated automatically by Diesel CLI.

diesel::table! {
    articles (sys_title) {
        id -> Int4,
        title -> Varchar,
        sys_title -> Varchar,
        published -> Bool,
        content -> Nullable<Jsonb>,
        do_aws_sync -> Nullable<Bool>,
        published_on -> Nullable<Int8>,
    }
}
