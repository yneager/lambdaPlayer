// Regression: home/addon rendering must never hide the top-menu installer.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../../resources/vui/stremio.js'), 'utf8');
const buttons = Array.from({length:3}, () => ({hidden:true, disabled:true, textContent:''}));
const elements = {
  '.addons-empty': {hidden:false},
  '.no-catalogs': null,
  '.addon-list': {innerHTML:'', appendChild() {}},
};
const context = {
  state:{addons:[], defaults:[], defaultsInstalled:false, installingDefaults:false},
  renderedAddons:null, bridge:{}, q: selector => elements[selector], qa: () => buttons,
  el() { return {}; }, addonCard() { return {}; }, renderServer() {}, refreshWindow() {},
};
vm.createContext(context);
for (const name of ['refreshDefaultButtons', 'updateHomeEmpty', 'renderAddons']) {
  const start=source.indexOf('  function '+name+'(');
  assert.ok(start>=0);
  const end=source.indexOf('\n  function ',start+1);
  vm.runInContext(source.slice(start, end<0 ? source.length : end), context);
}
for (const test of [
  {installed:false,busy:false,label:'Install default addons'},
  {installed:true,busy:false,label:'All installed'},
  {installed:false,busy:false,label:'Install default addons'},
  {installed:false,busy:true,label:'Installing…'},
]) {
  context.state.defaultsInstalled=test.installed;
  context.state.installingDefaults=test.busy;
  context.state.addons=test.installed ? [{id:'configured-torrentio'}] : [];
  context.updateHomeEmpty(1);
  context.renderAddons();
  for(const button of buttons){
    assert.equal(button.hidden,false);
    assert.equal(button.disabled,test.busy);
    assert.equal(button.textContent,test.label);
  }
}
console.log('PASS: default-addon buttons stay visible through home/addon renders, completed installs, removal and busy states.');
