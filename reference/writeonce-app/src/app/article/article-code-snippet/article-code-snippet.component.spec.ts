import { ComponentFixture, TestBed } from '@angular/core/testing';

import { ArticleCodeSnippetComponent } from './article-code-snippet.component';

describe('ArticleCodeSnippetComponent', () => {
  let component: ArticleCodeSnippetComponent;
  let fixture: ComponentFixture<ArticleCodeSnippetComponent>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [ArticleCodeSnippetComponent]
    })
    .compileComponents();
    
    fixture = TestBed.createComponent(ArticleCodeSnippetComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });
});
