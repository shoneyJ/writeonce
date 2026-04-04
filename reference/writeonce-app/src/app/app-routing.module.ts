import { NgModule } from '@angular/core';
import { RouterModule, Routes } from '@angular/router';
import { ArticleComponent } from './article/article.component';
import { AboutComponent } from './about/about.component';
import { ContactComponent } from './contact/contact.component';
import { HomeComponent } from './home/home.component';
import { RepositoryListComponent } from './repository-list/repository-list.component';
const routes: Routes = [
  { path: 'blog/:systitle', component: ArticleComponent },
  { path: 'projects', component: RepositoryListComponent },
  { path: 'about', component: AboutComponent }, 
  { path: 'contact', component: ContactComponent },
  { path: '', component: HomeComponent },  
  // other routes can go here
];


@NgModule({
  imports: [RouterModule.forRoot(routes)],
  exports: [RouterModule]
})
export class AppRoutingModule { }
