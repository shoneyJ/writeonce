import { ComponentFixture, TestBed } from '@angular/core/testing';

import { ScreenShotImageComponent } from './screen-shot-image.component';

describe('ScreenShotImageComponent', () => {
  let component: ScreenShotImageComponent;
  let fixture: ComponentFixture<ScreenShotImageComponent>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [ScreenShotImageComponent]
    })
    .compileComponents();
    
    fixture = TestBed.createComponent(ScreenShotImageComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });
});
