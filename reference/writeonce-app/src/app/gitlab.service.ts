import { Injectable } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable } from 'rxjs';

@Injectable({
  providedIn: 'root'
})
export class GitlabService {
  private baseUrl = 'https://git.writeonce.de/api/v4'; // GitLab API base URL


  constructor(private http: HttpClient) { }

  getProject(id: number): Observable<any> {
    return this.http.get<any>(`${this.baseUrl}/projects/${id}`);
  }

  getRepositoryTree(id: number): Observable<any> {
    return this.http.get<any>(`${this.baseUrl}/projects/${id}/repository/tree`);
  }

  getPublicRepositories(): Observable<any> {
    return this.http.get<any>(`${this.baseUrl}/projects?visibility=public`);
  }

  getRepositoryLanguages(id: number): Observable<any> {
    return this.http.get<any>(`${this.baseUrl}/projects/${id}/languages`);
  }
}
