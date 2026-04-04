pub mod article;
pub mod spec;
pub mod valid;
pub mod image;
use actix_web::web;

pub fn init_routes(cfg: &mut web::ServiceConfig) {
    cfg.service(article::get_article_by_id);
    cfg.service(article::upsert);
    cfg.service(article::get_articles_pagination);
    cfg.service(article::get_articles_total_count);
    cfg.service(article::get_article_by_title);
    cfg.service(article::remove_article_by_id);
    cfg.service(spec::openapi_spec);
    cfg.service(spec::swagger_ui);
    cfg.service(article::get_article_by_file_name);
    cfg.service(article::get_markdown_by_file_name);
    cfg.service(image::get_image_by_sys_title_file_name);
}
