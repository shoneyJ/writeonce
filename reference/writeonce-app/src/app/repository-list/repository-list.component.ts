import { Component, OnInit } from '@angular/core';
import { GitlabService } from '../gitlab.service';
import { IconDefinition, faJs, faPython, faJava, faHtml5, faCss3, faAngular, faReact, faNodeJs, faDocker, faSquareLastfm } from '@fortawesome/free-brands-svg-icons';
import { faQuestionCircle } from '@fortawesome/free-solid-svg-icons';


@Component({
  selector: 'app-repository-list',
  templateUrl: './repository-list.component.html',
  styleUrls: ['./repository-list.component.css']
})
export class RepositoryListComponent implements OnInit {
  repositories: any[] = [];
   // Mapping languages to Font Awesome icons
   TECH_ICONS: { [key: string]: IconDefinition } = {
    'JavaScript': faJs,
    'Python': faPython,
    'Java': faJava,
    'HTML': faHtml5,
    'CSS': faCss3,
    'Angular': faAngular,
    'React': faReact,
    'Node.js': faNodeJs,
    'Dockerfile': faDocker
  };

  constructor(private gitlabService: GitlabService) { }

  ngOnInit(): void {
    this.loadRepositories();
  }

  loadRepositories(): void {
    this.gitlabService.getPublicRepositories().subscribe(
      (data) => {
        this.repositories = data;

       // Iterate over each repository to get its languages
    this.repositories.forEach((r) => {
      this.gitlabService.getRepositoryLanguages(r.id).subscribe(
        (languagesData) => {
          // Assuming you want to store languages for each repository
      
            r.languages = languagesData;
            console.log(languagesData)
        },
        (error) => console.error(`Failed to get languages for repository ${r.Id}:`, error)
      );
    });
      },
      (error) => console.error(error)
    );
  }

  isValid(entry: { key: any, value: any }):boolean{
   
    return typeof entry.key === 'string';
  }

  getTechIcon(language: string): IconDefinition  {
    console.log('hit it')
    return this.TECH_ICONS[language] || faQuestionCircle;
  }
}
