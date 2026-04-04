import { Component, Input, ViewChildren, ViewContainerRef, QueryList, AfterViewInit } from '@angular/core';

import { ArticleCodeSnippetComponent } from '../article-code-snippet/article-code-snippet.component';
import { ScreenShotImageComponent } from '../screen-shot-image/screen-shot-image.component';

@Component({
  selector: 'app-article-section',
  templateUrl: './article-section.component.html',
  styleUrl: './article-section.component.css'
})
export class ArticleSectionComponent implements AfterViewInit {
 
  @Input() heading: string = '';
  @Input() paragraphs: string [] = [];
  @Input() sectionIndex: number= 0;
  @Input() codeSnippets: { language: string; snippet: string; title: string; sectionIndex: number, paragraphIndex: number }[] = [];
  @Input() images: { path: string; caption: string; sectionIndex: number, paragraphIndex: number }[] = [];

  @ViewChildren('dynamicCodeSnippetComponent', { read: ViewContainerRef }) codeSnippetcontainers!: QueryList<ViewContainerRef>;
  @ViewChildren('dynamicImageComponent', { read: ViewContainerRef }) ImageContainers!: QueryList<ViewContainerRef>;

  constructor() {}
  ngAfterViewInit(): void {
    this.addCodeSnippets();
    this.addImages();
  }

  addCodeSnippets() {
    this.codeSnippets.forEach(snippet => {
      if (this.sectionIndex === snippet.sectionIndex) {
        const container = this.codeSnippetcontainers.toArray()[snippet.paragraphIndex];

        if (container){
        const componentRef = container.createComponent(ArticleCodeSnippetComponent);
        componentRef.instance.inputMarkdownContentPath = snippet.snippet;
        componentRef.instance.codeLang = snippet.language;
        componentRef.instance.title = snippet.title;
        }
      }

    });
  }

  addImages() {
    this.images.forEach(image => {
      if (this.sectionIndex === image.sectionIndex) {
        const container = this.ImageContainers.toArray()[image.paragraphIndex];

        if (container){
        const componentRef = container.createComponent(ScreenShotImageComponent);
        componentRef.instance.imgSrc = image.path;
        componentRef.instance.caption = image.caption;
        }     

      }
      

    });
  }

}
