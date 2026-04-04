import { Component, OnInit, Input } from '@angular/core';
import { ArticleService } from '../services/article.service';
import { ArticleContent, Articles } from '../models/article';
import { ArticleCodeSnippetComponent } from '../article/article-code-snippet/article-code-snippet.component';


@Component({
  selector: 'app-home',
  templateUrl: './home.component.html',
  styleUrl: './home.component.css'
})




export class HomeComponent implements OnInit {
 
  @Input() articles : {router:string, introduction: string, title:string,tags: string[]}[] =[];
  searchTerm: string = '';
  filteredArticles : {router:string, introduction: string, title:string, tags: string[]}[] =[];
  paginatedArticles :  {router:string, introduction: string, title:string, tags: string[]}[] =[];
  currentPage: number = 1;
  itemsPerPage: number = 5; 
  totalArticles: number = 0;

  constructor( private dbService: ArticleService) {
  }


 async ngOnInit(): Promise<void> {
  this.dbService.getArticlesCount().subscribe((resp)=>{
    this.totalArticles = resp.count;
  })       
    this.updatePagination();
  }

  async getArticlePage(skip: number, limit: number){

   return  this.dbService.getArticlesPagination(skip,limit).subscribe((resp : ArticleContent[])  =>{

   this.articles = resp.map(r => new Articles(r).toArticlePageModel());

   });

  }

  filterArticles() {
    if (!this.searchTerm) {
      this.filteredArticles = this.articles;
    } else {
      this.filteredArticles = this.articles.filter(article =>
        article.title.toLowerCase().includes(this.searchTerm.toLowerCase()) ||
        article.introduction.toLowerCase().includes(this.searchTerm.toLowerCase())
      );
    }
    this.currentPage = 1; // Reset to the first page on new search
    this.updatePagination();
  }

  updatePagination() {
    const start = (this.currentPage - 1) * this.itemsPerPage;
    this.getArticlePage(start,this.itemsPerPage);
  }

  nextPage() {
    if (this.currentPage < this.totalPages) {
      this.currentPage++;
      this.updatePagination();
    }
  }

  previousPage() {
    if (this.currentPage > 1) {
      this.currentPage--;
      this.updatePagination();
    }
  }

  get totalPages() {
    return Math.ceil(this.totalArticles / this.itemsPerPage);
  }

}
