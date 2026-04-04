import { Component, Input, OnInit } from '@angular/core';

@Component({
  selector: 'app-article-signature',
  templateUrl: './article-signature.component.html',
  styleUrl: './article-signature.component.css'
})
export class ArticleSignatureComponent  implements OnInit{
  publishedDate: Date = new Date();
  ngOnInit(): void {
    this.publishedDate = new Date(this.publishedTimestamp * 1000); // Convert to Date object
  
  }
  @Input() authorName: string = '';
  @Input() publishedTimestamp: number =0;
}
