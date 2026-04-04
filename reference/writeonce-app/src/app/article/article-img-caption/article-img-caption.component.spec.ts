import { ComponentFixture, TestBed } from '@angular/core/testing';

import { ArticleImgCaptionComponent } from './article-img-caption.component';

describe('ArticleImgCaptionComponent', () => {
  let component: ArticleImgCaptionComponent;
  let fixture: ComponentFixture<ArticleImgCaptionComponent>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [ArticleImgCaptionComponent]
    })
    .compileComponents();
    
    fixture = TestBed.createComponent(ArticleImgCaptionComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });
});
