import { TestBed } from '@angular/core/testing';

import { WriteoncedbService } from './writeoncedb.service';

describe('WriteoncedbService', () => {
  let service: WriteoncedbService;

  beforeEach(() => {
    TestBed.configureTestingModule({});
    service = TestBed.inject(WriteoncedbService);
  });

  it('should be created', () => {
    expect(service).toBeTruthy();
  });
});
