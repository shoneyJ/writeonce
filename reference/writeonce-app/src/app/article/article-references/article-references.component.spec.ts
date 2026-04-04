import { ComponentFixture, TestBed } from '@angular/core/testing';

import { ArticleReferencesComponent } from './article-references.component';

describe('ArticleReferencesComponent', () => {
  let component: ArticleReferencesComponent;
  let fixture: ComponentFixture<ArticleReferencesComponent>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [ArticleReferencesComponent]
    })
    .compileComponents();
    
    fixture = TestBed.createComponent(ArticleReferencesComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });
});
