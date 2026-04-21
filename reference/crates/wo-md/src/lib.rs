mod block;
mod inline;
mod highlight;

pub use block::markdown_to_html_block;
pub use inline::markdown_to_html;
pub use highlight::highlight;
