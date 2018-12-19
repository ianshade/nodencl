/* Copyright 2018 Streampunk Media Ltd.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

const addon = require('../index.js');
const tape = require('tape');

const pi = 0;
const di = 0;
const properties = { platformIndex: pi, deviceIndex: di };
function createContext(description, cb) {
  tape(description, async t => {
    const clContext = new addon.clContext(properties);
    try {
      await cb(t, clContext);
      clContext.close(t.end);
    } catch (err) {
      t.fail(err);
      t.end();
    }
  });
}

const testKernel = `
  __kernel void test(__global uint4* restrict input,
                     __global uint4* restrict output) {
    uint off = (get_group_id(0) * get_local_size(0) + get_local_id(0)) * 4;
    for (uint i=0; i<4; ++i) {
      output[off] = input[off];
      ++off;
    }
  }
`;

const width = 1024;
const height = 64;
const numBytes = width * height * 4; // rgba8
const createProgram = function(clContext, kernel) {
  // process one image line per work group, 16 pixels of rgba8 per work item
  const workItemsPerGroup = width / 16;
  const globalWorkItems = workItemsPerGroup * height;
  return clContext.createProgram(kernel, {
    name: 'test',
    globalWorkItems: globalWorkItems,
    workItemsPerGroup: workItemsPerGroup
  });
};

createContext('Run OpenCL program with buffer parameters', async (t, clContext) => {
  const testProgram = await createProgram(clContext, testKernel);
  const srcBuf = Buffer.alloc(numBytes);
  for (let i=0; i<numBytes; i+=4)
    srcBuf.writeUInt32LE((i/4)&0xff, i);

  const bufIn = await clContext.createBuffer(numBytes, 'readonly', 'none');
  await bufIn.hostAccess('writeonly', srcBuf);
  const bufOut = await clContext.createBuffer(numBytes, 'writeonly', 'none');

  await testProgram.run({ input: bufIn, output: bufOut });
  await bufOut.hostAccess('readonly');
  t.deepEqual(bufOut, bufIn, 'program produced expected result');
});

createContext('Run OpenCL program with missing parameter', async (t, clContext) => {
  const testProgram = await createProgram(clContext, testKernel);
  const bufIn = await clContext.createBuffer(numBytes, 'readonly', 'none');
  try {
    await testProgram.run({ input: bufIn });
    t.fail('missing parameter should give error');
  } catch (err) {
    t.pass(`missing parameter produces ${err}`);
  }
});

createContext('Run OpenCL program with extra parameter', async (t, clContext) => {
  const testProgram = await createProgram(clContext, testKernel);
  const bufIn = await clContext.createBuffer(numBytes, 'readonly', 'none');
  const bufOut = await clContext.createBuffer(numBytes, 'writeonly', 'none');
  try {
    await testProgram.run({ input: bufIn, output: bufOut, extra: 'unexpected' });
    t.fail('extra parameter should give error');
  } catch (err) {
    t.pass(`extra parameter produces ${err}`);
  }
});
