import { Component, OnInit, Input } from '@angular/core';
import { ArticleReference } from './article-reference.model';


@Component({
  selector: 'app-article-references',
  templateUrl: './article-references.component.html',
  styleUrl: './article-references.component.css'
})


export class ArticleReferencesComponent implements OnInit {
  @Input()
  references: ArticleReference[] = [];
  ngOnInit(): void {
    this.references = this.references.map(reference => ({
      ...reference,
      accessedOnDate: new Date(reference.dateAccessed * 1000) // Convert Unix timestamp to Date
    }));
  }

}
