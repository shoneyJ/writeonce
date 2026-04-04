import { Component, Input, ViewEncapsulation } from '@angular/core';

@Component({
  selector: 'article-img-caption',
  templateUrl: './article-img-caption.component.html',
  styleUrl: './article-img-caption.component.css',
  encapsulation: ViewEncapsulation.None 
})
export class ArticleImgCaptionComponent {
  @Input() imgSrc: string = '';
  @Input() caption: string = '';
}
