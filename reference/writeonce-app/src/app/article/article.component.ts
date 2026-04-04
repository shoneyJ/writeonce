import { AfterViewInit, Component, OnInit } from '@angular/core';
import { ActivatedRoute } from '@angular/router';
import { ArticleService } from '../services/article.service';
import { Meta, Title } from '@angular/platform-browser';
import { Subscription } from 'rxjs';
import { ArticleModel, Articles, ArticleContent } from '../models/article';

@Component({
  selector: 'app-article',
  templateUrl: './article.component.html',
  styleUrl: './article.component.css'
})
export class ArticleComponent implements OnInit, AfterViewInit {

  article: ArticleModel | undefined;

  constructor(
    private route: ActivatedRoute,
    private articleService: ArticleService,
    private meta: Meta,
    private headTitle: Title
  ) { }
  ngAfterViewInit(): void {
    this.setMetaTitle()
  }

  ngOnInit(): void {

    this.route.paramMap.subscribe(async (params) => {
      const title = params.get('systitle');
      if (title) {
        await this.loadBlogPost(title);
      }

    });
  }

  private setMetaTitle() {
    if (this.article) {
      this.headTitle.setTitle(this.article.title);

      // Check if the article has tags and update meta keywords
      if (this.article.tags && this.article.tags.length > 0) {
        const keywords = this.article.tags.join(', ');
        this.meta.updateTag({ name: 'keywords', content: keywords });
      } else {
        // Optionally, remove the keywords tag if no tags are present
        this.meta.removeTag('name="keywords"');
      }

    }

  }



  private async loadBlogPost(systitle: string): Promise<Subscription> {

    return new Promise((resolve, reject) => {
      const res = this.articleService.getArticlesBySysTitle(systitle).subscribe((resp: ArticleContent) => {
        // Find the article by title
        this.article = new Articles(resp)
          .toArticleModel();
      });
      resolve(res);


    });

  }

}
