import { ComponentFixture, TestBed } from '@angular/core/testing';

import { ArticleSignatureComponent } from './article-signature.component';

describe('ArticleSignatureComponent', () => {
  let component: ArticleSignatureComponent;
  let fixture: ComponentFixture<ArticleSignatureComponent>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [ArticleSignatureComponent]
    })
    .compileComponents();
    
    fixture = TestBed.createComponent(ArticleSignatureComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });
});
