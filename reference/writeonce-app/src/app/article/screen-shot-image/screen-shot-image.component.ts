import { Component ,Input } from '@angular/core';

@Component({
  selector: 'app-screen-shot-image',
  templateUrl: './screen-shot-image.component.html',
  styleUrl: './screen-shot-image.component.css'
})
export class ScreenShotImageComponent {
  @Input() imgSrc: string = '';
  @Input() caption: string = '';

}
