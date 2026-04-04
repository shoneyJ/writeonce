/// A parsed template node.
#[derive(Debug, Clone, PartialEq)]
pub enum Node {
    /// Raw HTML passthrough.
    Literal(String),
    /// Variable binding: `{{path.to.value}}`
    Binding(Vec<String>),
    /// Iteration: `{{#each path}} ... {{/each}}`
    Each {
        path: Vec<String>,
        body: Vec<Node>,
    },
    /// Partial inclusion: `{{> name arg=value}}`
    Partial {
        name: String,
        args: Vec<(String, Vec<String>)>,
    },
}
