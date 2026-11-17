#!/usr/bin/env zx
// Exact wrapper environment/argv route; only a local Node process is started.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {sceneLaunch, asyncRunner} from './android-scene.mjs';

test('one scene route binds the six inputs and all cleanup deadlines to the same start',async()=>{
  const root=process.env.SCENE_ROUTE_TEST_OUT;
  assert(root&&path.isAbsolute(root),'isolated local evidence directory required');
  fs.mkdirSync(root,{recursive:true});
  const out=fs.mkdtempSync(path.join(root,'route-'));
  const t0=Date.now();
  const launch=sceneLaunch(out,'/unused-sdk',{...process.env,
    SCENE_OBSERVER:'stale-shell-observer',SCENE_DEADLINE_MS:'invalid inherited value'},t0);
  assert.equal(launch.argv[0],process.execPath);
  assert(launch.argv[1].endsWith('/android-scene/scene.mjs'));
  assert.equal(launch.argv[2],'execute');assert.equal(launch.argv.length,3);
  assert.equal(launch.bindings.SCENE_DEADLINE_MS,String(t0+270000));
  assert.equal(launch.env.SCENE_DEADLINE_MS,String(t0+270000));
  assert.equal(launch.hardDeadline,t0+300000);
  assert.equal(launch.limits.naturalUntilMs,t0+275000);
  assert.equal(launch.limits.deadlineMs,t0+285000);
  assert.equal(launch.limits.termGraceMs,5000);assert.equal(launch.limits.killGraceMs,5000);
  assert(!('SCENE_OBSERVER' in launch.env));
  assert.notEqual(launch.env.RUNTIME_SO,launch.env.HOST_SYMBOLS);
  assert(launch.env.RUNTIME_SO.includes('/readscope-android-scene-artifact/x86_64/install/'));
  const required=JSON.parse(fs.readFileSync(new URL('./android-scene/job-interface.json',import.meta.url))).requiredEnv;
  assert.deepEqual(Object.keys(launch.bindings).sort(),[...required].sort());
  // Pass the exact production environment through the real wrapper runner,
  // substituting only an inert Node env reader for the forbidden native scene.
  const run=asyncRunner(out,launch.env);
  const result=await run('local-route-env',[process.execPath,'-e',
    'console.log(JSON.stringify({deadline:process.env.SCENE_DEADLINE_MS,runtime:process.env.RUNTIME_SO,observer:process.env.SCENE_OBSERVER??null}));process.exit(19)'],
  {naturalUntilMs:t0+1500,deadlineMs:t0+2000,termGraceMs:100,killGraceMs:100});
  assert.equal(result.rc,19);assert.equal(result.signal,null);
  assert.equal(result.reaped,true);assert.equal(result.timedOut,false);
  const seen=JSON.parse(fs.readFileSync(path.join(out,'local-route-env.stdout')));
  assert.equal(seen.deadline,String(t0+270000));assert.equal(seen.runtime,launch.env.RUNTIME_SO);
  assert.equal(seen.observer,null);
  fs.writeFileSync(path.join(out,'route.json'),JSON.stringify({argv:launch.argv,bindings:launch.bindings,
    sceneStarted:t0,hardDeadline:launch.hardDeadline,limits:launch.limits,result,nativeRuns:0},null,2)+'\n');
});
