import { provideHttpClient, withInterceptorsFromDi } from '@angular/common/http';
import { NgModule } from '@angular/core';
import { BrowserModule } from '@angular/platform-browser';
import { RouterLink, RouterLinkActive } from '@angular/router';

import { AppRoutingModule } from './app-routing.module';
import { AppComponent } from './app.component';
import { NavigationBarComponent } from './navigation-bar/navigation-bar.component';
import { ArticleComponent } from './article/article.component';
import { HeaderComponent } from './header/header.component';
import { FooterComponent } from './footer/footer.component';
import { AboutComponent } from './about/about.component';
import { ContactComponent } from './contact/contact.component';
import { HomeComponent } from './home/home.component';
import { RepositoryListComponent } from './repository-list/repository-list.component';
import { FaIconLibrary, FontAwesomeModule } from '@fortawesome/angular-fontawesome';

import { faEnvelope, faPhone } from '@fortawesome/free-solid-svg-icons';
import { faLinkedin as faLinkedinBrands } from '@fortawesome/free-brands-svg-icons';
import { FormsModule } from '@angular/forms';
import { ArticleImgCaptionComponent } from './article/article-img-caption/article-img-caption.component';
import { ArticleSectionComponent } from './article/article-section/article-section.component';
import { ArticleSignatureComponent } from './article/article-signature/article-signature.component';
import { ArticleReferencesComponent } from './article/article-references/article-references.component';
import { ArticleCodeSnippetComponent } from './article/article-code-snippet/article-code-snippet.component';
import { ScreenShotImageComponent } from './article/screen-shot-image/screen-shot-image.component';
import { SummaryCardComponent } from './article/summary-card/summary-card.component';
import { MarkdownModule } from 'ngx-markdown';



@NgModule({ declarations: [
        AppComponent,
        NavigationBarComponent,
        ArticleComponent,
        HeaderComponent,
        FooterComponent,
        AboutComponent,
        ContactComponent,
        HomeComponent,
        RepositoryListComponent,
        ArticleImgCaptionComponent,
        ArticleSectionComponent,
        ArticleSignatureComponent,
        ArticleReferencesComponent,
        ArticleCodeSnippetComponent,
        ScreenShotImageComponent,
        SummaryCardComponent
    ],
    bootstrap: [AppComponent],
     imports: [BrowserModule,
        AppRoutingModule,
        FontAwesomeModule,
        FormsModule,
        RouterLink,
        RouterLinkActive,
        MarkdownModule.forRoot()],
     providers: [provideHttpClient(withInterceptorsFromDi())] })
export class AppModule { 
  constructor(library: FaIconLibrary) {
    library.addIcons(faEnvelope, faPhone, faLinkedinBrands);
  }
}
