import { Component, AfterViewInit, Input, OnInit } from '@angular/core';
import { Clipboard } from '@angular/cdk/clipboard';
import { faCopy } from '@fortawesome/free-solid-svg-icons';
import { faCheck } from '@fortawesome/free-solid-svg-icons';
import { ArticleService } from '../../services/article.service';
import { marked } from 'marked';
import Prism from 'prismjs';

@Component({
  selector: 'app-article-code-snippet',
  templateUrl: './article-code-snippet.component.html',
  styleUrl: './article-code-snippet.component.css'
})
export class ArticleCodeSnippetComponent implements OnInit {

  /**
   *
   */
  @Input() inputMarkdownContentPath: string = ``;

  @Input() title: string = "";
  @Input() codeLang: string = "";

  public markdownContent: string = '';
  public faCopy = faCopy;

  public copyText: string = "";
  public codeBlock: string = "";
  constructor(
    private clipboard: Clipboard,
    private articleService: ArticleService
  ) { }
  ngOnInit(): void {
    this.initCopyBtn();
    this.markdownContentPath = this.inputMarkdownContentPath;
    this.configureMarkedRenderer();
  }

  // ngAfterViewInit(): void {
  //    // Highlight the code after view initialization
  //    (window as any).Prism.highlightAll();
  // }

  configureMarkedRenderer() {
    const renderer = new marked.Renderer();

    // Override the code block renderer to use PrismJS for syntax highlighting
    renderer.code = (code: string, language: string) => {
      this.codeBlock = code;
      const validLanguage = Prism.languages[language] ? language : 'markup';
      const highlightedCode = Prism.highlight(code, Prism.languages[validLanguage], validLanguage);
      return `<pre class="language-${validLanguage}"><code class="language-${validLanguage}">${highlightedCode}</code></pre>`;
    };

    marked.setOptions({
      renderer
    });
  }


  private set markdownContentPath(path: string) {

    this.articleService.getMarkdown(path).subscribe(async (data) => {
      this.markdownContent = await marked(data); // Convert Markdown to HTML
    });

  }

  copyCode() {
    this.clipboard.copy(this.codeBlock);
    this.copyText = "Copied!";
    this.faCopy = faCheck;

    setTimeout(() => {
      this.initCopyBtn();
    }, 2000);
  }

  initCopyBtn() {

    this.copyText = "Copy";
    this.faCopy = faCopy;

  }



}
