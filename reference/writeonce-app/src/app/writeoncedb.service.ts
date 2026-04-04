import { Injectable, OnInit } from '@angular/core';
import { WebsocketService } from './websocket.service';
import { Json } from './types';

@Injectable({
  providedIn: 'root'
})
export class WriteoncedbService implements OnInit {
  private collection: String;
  private websocketService: WebsocketService;

  constructor(_collection: String) {
    this.collection = _collection;
    this.websocketService = new WebsocketService()
  }
  ngOnInit(): void {
    // The WebSocket connection is automatically established when the service is instantiated
  }

  insert(document: string,json: Json): void {
    const data = JSON.stringify(json);
    const message = `insert into ${this.collection} of doc id:${document}; json =${data}.`;
    this.websocketService.sendCustomMessage(message);
    this.closeConnection();
  }

  closeConnection(): void {
    this.websocketService.closeConnection();
  }

  convertTextToDocId(text: string): string {
    return text
      .toLowerCase()
      .trim()
      .replace(/[^a-z0-9]+/g, '-')
      .replace(/^-+|-+$/g, '');
  }
}
