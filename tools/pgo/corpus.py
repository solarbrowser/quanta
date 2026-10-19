# Writes one self-contained training script per npm library (the library followed by a workload) into OUT.
# usage: corpus.py <dir containing node_modules> <out dir>
import os, sys
N = os.path.join(sys.argv[1], 'node_modules') + '/'
OUT = sys.argv[2]
def wrap(path):
    src=open(N+path if not path.startswith('/') else path).read()
    return "var __m={exports:{}};(function(module,exports,define){\n"+src+"\n}).call(__m.exports,__m,__m.exports,undefined);var LIB=__m.exports;\n"
SHIM="var window=undefined,self=undefined;\nvar __sum=0;function chk(s){s=String(s);for(var i=0;i<s.length;i++)__sum=(__sum*31+s.charCodeAt(i))|0;}\n"
D={}
D['lodash']=("lodash/lodash.js","""
var _=LIB;
for(var r=0;r<40;r++){
 var data=_.range(0,2000).map(function(i){return {id:i,g:i%17,name:'item'+i,tags:['a'+(i%5),'b'+(i%7)],v:(i*7919)%1000};});
 var grouped=_.groupBy(data,'g');var sorted=_.orderBy(data,['g','v'],['asc','desc']);
 chk(_.sumBy(data,'v'));chk(_.keys(grouped).length);chk(sorted[5].id);
 chk(JSON.stringify(_.chunk(_.uniq(_.flatMap(data,'tags')),3)));
 chk(_.cloneDeep(data[10]).name);chk(_.debounce.length);
 chk(_.template('<% _.forEach(xs,function(x){ %><li><%- x %></li><% }); %>')({xs:['a','<b>','c']}));
 chk(_.merge({},{a:{b:1}},{a:{c:2}}).a.c);chk(_.pick(data[3],['id','v']).v);chk(_.mapValues(grouped,function(v){return v.length})[3]);
 chk(_.isEqual(data[1],_.cloneDeep(data[1])));chk(_.zip([1,2],[3,4]).length);chk(_.capitalize('hello')+_.camelCase('foo bar baz')+_.snakeCase('Foo Bar'));
}
""")
D['moment']=("moment/moment.js","""
var moment=LIB;
for(var r=0;r<3000;r++){
 var m=moment.utc('2024-01-15T10:30:00Z').add(r,'hours');
 chk(m.format('YYYY-MM-DD HH:mm:ss dddd MMMM'));chk(m.startOf('month').diff(moment.utc('2020-01-01'),'days'));
 chk(m.clone().subtract(3,'weeks').isBefore(m));chk(moment.utc('2020-02-29').add(r%400,'days').format('LL'));chk(m.fromNow?1:0);
 chk(moment.duration(r,'minutes').humanize());chk(moment.utc(String(2000+r%30)+'-03-'+(10+r%15),'YYYY-MM-DD').week());
}
""")
D['marked']=("marked/lib/marked.umd.js","""
var md='# Title\\n\\nSome *emphasis* and **strong** text with `code` and [link](http://x.y/z).\\n\\n- one\\n- two\\n  - nested\\n- three\\n\\n1. a\\n2. b\\n\\n> quote line\\n> more\\n\\n```js\\nvar a = 1;\\nfunction f(){ return a; }\\n```\\n\\n| a | b |\\n|---|---|\\n| 1 | 2 |\\n\\nText with <b>html</b> &amp; entities.\\n';
var marked=LIB.marked||LIB;
for(var r=0;r<600;r++){ var s=''; for(var k=0;k<4;k++) s+=md.replace('Title','T'+r+'_'+k); chk(marked.parse(s)); }
""")
D['handlebars']=("handlebars/dist/handlebars.js","""
var H=LIB;
H.registerHelper('up',function(s){return String(s).toUpperCase();});
var tpl=H.compile('<ul>{{#each items}}<li class="{{cls}}">{{up name}} - {{price}} {{#if sale}}<b>SALE</b>{{else}}reg{{/if}}</li>{{/each}}</ul><p>{{{raw}}} {{title}}</p>');
for(var r=0;r<400;r++){ var items=[];for(var i=0;i<60;i++)items.push({cls:'c'+(i%3),name:'n'+i+'&<'+r,price:i*1.5,sale:i%2==0});
 chk(tpl({items:items,raw:'<i>x</i>',title:'T'+r}));
 chk(H.compile('Hello {{name}} {{#each a}}{{this}},{{/each}}')({name:'w'+r,a:[1,2,3]})); }
""")
D['acorn']=("acorn/dist/acorn.js","""
var acorn=LIB;var code=%s;
for(var r=0;r<40;r++){ var ast=acorn.parse(code+'\\n// '+r,{ecmaVersion:2022,locations:true}); chk(ast.body.length); var n=0;(function w(o){if(o&&typeof o==='object'){if(o.type)n++;for(var k in o)if(k!=='loc')w(o[k]);}})(ast);chk(n); }
""")
D['esprima']=("esprima/dist/esprima.js","""
var esprima=LIB;var code=%s;
for(var r=0;r<30;r++){ var ast=esprima.parseScript(code+'\\n// '+r,{range:true,loc:true}); chk(ast.body.length); chk(esprima.tokenize(code.slice(0,3000)).length); }
""")
D['immutable']=("immutable/dist/immutable.js","""
var I=LIB;
for(var r=0;r<30;r++){ var m=I.Map();for(var i=0;i<3000;i++)m=m.set('k'+i,{v:i});
 var l=I.List();for(var i=0;i<3000;i++)l=l.push(i*r);
 chk(m.size);chk(l.filter(function(x){return x%3==0}).map(function(x){return x+1}).reduce(function(a,b){return a+b},0));
 chk(m.get('k77').v);chk(I.fromJS({a:[1,{b:2}]}).getIn(['a',1,'b']));chk(I.Set(l).size);chk(m.toList().size);chk(I.OrderedMap(m).keySeq().first()); }
""")
D['papaparse']=("papaparse/papaparse.js","""
var P=LIB;var rows=['id,name,val,note'];for(var i=0;i<2000;i++)rows.push(i+',"name, '+i+'",'+(i*1.5)+',"say ""hi"" '+i+'"');var csv=rows.join('\\n');
for(var r=0;r<40;r++){ var res=P.parse(csv,{header:true,dynamicTyping:true}); chk(res.data.length);chk(res.data[5].name); chk(P.unparse(res.data.slice(0,300)).length); }
""")
D['ejs']=("ejs/ejs.js","""
var ejs=LIB;
var t='<% items.forEach(function(it){ %><div class="<%= it.cls %>"><%- it.html %> <%= it.name %></div><% }) %>';
for(var r=0;r<300;r++){ var items=[];for(var i=0;i<60;i++)items.push({cls:'c'+i,html:'<b>'+i+'</b>',name:'n<'+r});
 chk(ejs.render(t,{items:items})); chk(ejs.compile('Hi <%= n %>')({n:r})); }
""")
D['mustache']=("mustache/mustache.js","""
var M=LIB;var t='{{#items}}<li>{{name}} {{#sub}}<i>{{.}}</i>{{/sub}}{{^sub}}none{{/sub}}</li>{{/items}}{{title}}';
for(var r=0;r<600;r++){ var items=[];for(var i=0;i<50;i++)items.push({name:'n'+i,sub:i%2?['a','b']:[]}); chk(M.render(t,{items:items,title:'T'+r})); }
""")
D['dayjs']=("dayjs/dayjs.min.js","""
var d=LIB;
for(var r=0;r<20000;r++){ var x=d('2024-03-10T12:00:00Z').add(r,'minute'); chk(x.format('YYYY-MM-DD HH:mm'));chk(x.diff(d('2020-01-01T00:00:00Z'),'day'));chk(x.startOf('week').valueOf()); }
""")
# source for parser drivers
CODE="""(function(){var s='';for(var i=0;i<120;i++){s+='function f'+i+'(a,b){var x={p:a,q:[1,2,3],r:function(z){return z*'+i+'}};for(var k=0;k<10;k++){if(a>b){x.p+=k}else{x.q.push(k)}} return `t${a}-${b}` + x.r(3) + (a?b:0);}\\nclass C'+i+' extends Object{constructor(a){super();this.a=a} m(){return this.a+'+i+'} static s(){return async()=>{await 1}}}\\n';}return s;})()"""
for k in ('acorn','esprima'):
    p,d=D[k]; D[k]=(p,d.replace('%s',CODE))
# typescript: transpile + parse
D['typescript']=("typescript/lib/typescript.js","""
var ts=LIB;var code=%s;
for(var r=0;r<6;r++){ var out=ts.transpileModule(code.replace(/function/g,'function')+'\\nexport const z'+r+': number = '+r+';',{compilerOptions:{target:ts.ScriptTarget.ES2015,module:ts.ModuleKind.CommonJS}}); chk(out.outputText.length);
 var sf=ts.createSourceFile('a.ts','interface I{a:number;b?:string}\\nclass K<T extends I> implements I{ a=1; constructor(public x:T){} m<U>(y:U):U{return y} }\\nconst v:Array<I>=[{a:1}];\\nenum E{A,B}\\nfunction g(x:number|string):x is number{return typeof x==="number"}\\n'+code.slice(0,6000),ts.ScriptTarget.ES2020,true); chk(sf.statements.length);
 (function w(n){chk(n.kind);ts.forEachChild(n,w);})(sf); }
""".replace('%s',CODE))
for k,(p,d) in D.items():
    src=SHIM+wrap(p)+d+"\nconsole.log('"+k+" '+__sum);\n"
    open(os.path.join(OUT, k + '.js'), 'w').write(src)
print(len(D))
