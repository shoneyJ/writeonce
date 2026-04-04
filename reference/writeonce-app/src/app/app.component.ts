import { HttpClient } from '@angular/common/http';
import { Component, OnInit } from '@angular/core';

interface WeatherForecast {
  date: string;
  temperatureC: number;
  temperatureF: number;
  summary: string;
}

interface UserDetail {
name: string
}


@Component({
  selector: 'app-root',
  templateUrl: './app.component.html',
  styleUrl: './app.component.css'
})
export class AppComponent implements OnInit {
  public users: UserDetail[] = [];

  constructor(private http: HttpClient) {}

  ngOnInit() {
   // this.getForecasts();
  }

  // getForecasts() {
  //   this.http.get<UserDetail[]>('api/v1.0/UserDetail').subscribe(
  //     (result) => {
  //       this.users = result;
  //     },
  //     (error) => {
  //       console.error(error);
  //     }
  //   );
  // }

  title = 'Write Once | Blog';
}
