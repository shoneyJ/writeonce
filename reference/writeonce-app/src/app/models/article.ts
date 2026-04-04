export interface ArticlePage {
    title: string;
    router:string;
    systitle: string;
    introduction :string;
    tags : string [];
  }

  export interface ArticleModel {
    title: string,
    content :{
        img: Image;
        sections: Section[];
        codes: Code[];
        images: Image[];
        };
    author: string;
    publishedOn: number;
    references: Reference[];
    tags : string [];
  }

export  interface ArticleContent {
    id: number;
    title: string;
    sys_title: string;
    content: Content;  
}

export interface Content {
    
   
    content :{
    img: Image;
    sections: Section[];
    codes: Code[];
    images: Image[];
    },
    tags : string [];
    author: string;
    publishedOn: number;
    references: Reference[];
}

interface Image {
    path: string;
    caption: string;
    sectionIndex: number;
    paragraphIndex: number;
}

interface Section {
    heading: string;
    paragraphs: string[];
}

interface Code {
    language: string;
    snippet: string;
    title: string;
    sectionIndex: number;
    paragraphIndex: number;
}

export interface Reference {
    title: string;
    url: string;
    dateAccessed: number;
}


export class Articles implements ArticleContent {
    id : number;
    title: string;
    sys_title: string;
    content: Content;
    author: string;
    publishedOn: number;
    references: Reference[];
    constructor(data: ArticleContent) {
        this.id = data.id
        this.title = data.title;
        this.sys_title = data.sys_title;
        this.content = data.content;
        this.author =this.content.author;
        this.publishedOn = this.content.publishedOn;
        this.references =this.content.references;
    }
    toArticlePageModel(): ArticlePage {

        const introduction = this.content.content.sections[0].paragraphs[0];
        const tags = this.content.tags;
       
       
        return {
            title: this.title,
            router : `blog/${this.sys_title}`,
            introduction,
            systitle: this.sys_title,
            tags,
          
        };
    }

    toArticleModel() : ArticleModel {

        return {
            title : this.title,
            author: this.content.author,
            publishedOn: this.publishedOn,
            references: this.content.references,
            tags: this.content.tags,
            content: this.content.content,
        }

    }
    
}


