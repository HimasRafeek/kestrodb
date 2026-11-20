// Included inside class Store (kestro.hpp): ordered keys, secondary indexes, queries,
// atomic increments. See QUERY.md.
//
// Locking. Everything here changes only with the writer lock held AND, while visible to
// queries, under readers_ exclusively (the same section that installs a commit). Queries
// hold readers_ shared for their whole run, so a query sees one committed state. Before a
// query or catalog build the index is materialized (lazy_ false), after which shard
// indexes, too, change only under readers_ exclusively, so queries read them unlocked.
//
// Built lazily: the ordered key set on the first query, a secondary index on the first
// query of its collection (or the first write, for unique indexes). A database that never
// queries pays nothing. Index definitions live in one reserved key, read at first need.
// First builds run without the writer lock (prebuild, below): commits go on meanwhile.
private:
 // A composite index (ON t (a, b, ...)) has its columns in cols and their paths in cpaths;
 // its field reads "a,b" and path is empty. Its entries are encode(a) + encode(b) + ... +
 // row key, a missing value encoded as null: both are SQL NULL, so the entry order is the
 // order of ORDER BY a, b. A one-column index has cols empty.
 // A FULLTEXT index (fulltext, store_fulltext.inc) has its columns in cols (one or more) and
 // other entries; no plan for plain conditions or ORDER BY ever uses it.
 struct SecIndex {std::string name,collection,field;std::vector<std::string> path;bool unique=false,built=false;SortedSet set;std::vector<std::string> cols;std::vector<std::vector<std::string>> cpaths;
  bool fulltext=false;size_t min_len=1;uint64_t ft_docs=0,ft_tokens=0;};  // ft_: documents with tokens and their tokens (BM25), kept by commits
 bool cat_loaded_=false,ordered_built_=false;
 std::vector<std::string> tables_;  // SQL tables (CREATE TABLE)
 std::map<std::string,std::vector<sql::ColumnDef>> columns_;  // declared columns per table (empty: schemaless)
 static std::string column_json(const sql::ColumnDef&c){
  std::string s="{\"name\":"+json::quote(c.name)+",\"type\":"+json::quote(c.type)+",\"notNull\":"+(c.not_null?"true":"false");
  if(c.has_default)s+=",\"default\":"+sql::to_json(c.def);
  if(c.length)s+=",\"length\":"+std::to_string(c.length);
  return s+"}";
 }
 std::vector<std::unique_ptr<SecIndex>> sec_;
 SortedSet ordered_;
 static std::string_view catalog_key(){return std::string_view("\x01" "catalog",8);}
 static std::string_view collection_of(std::string_view id){auto z=id.find('\0');return z==std::string_view::npos?std::string_view():id.substr(0,z);}
 bool has_index(std::string_view coll)const{if(coll.empty())return false;for(const auto&ix:sec_)if(ix->collection==coll)return true;return false;}
 // A value checked against its read checksum without copying it when it is mapped.
 std::string_view view_ref(const Ref&ref,std::string&buf)const{
  if(const char*m=ref.mapped?ref.mapped:addr_of(ref.offset,ref.length)){require(value_matches(key_,m,ref.length,ref.hash),"CORRUPT value on disk");return std::string_view(m,ref.length);}
  buf.resize(ref.length);if(ref.length)platform::read_exact(fd_.n,buf.data(),ref.length,ref.offset);
  require(value_matches(key_,buf.data(),buf.size(),ref.hash),"CORRUPT value on disk");return buf;
 }
 // Current committed value of id, as the writer sees it. Writer lock held.
 std::optional<std::string> current_value(std::string_view id)const{
  Ref r;auto h=index_hash(key_,id);if(!lookup(shard_of(h),id,h,r))return std::nullopt;return load_ref(r).value;
 }
 void load_catalog_locked(){
  if(cat_loaded_)return;
  if(auto v=current_value(catalog_key())){
   auto j=json::parse(*v);
   if(const auto*list=j.get("indexes"))for(const auto&it:list->items){
    auto ix=std::make_unique<SecIndex>();
    const auto*c=it.get("collection");const auto*f=it.get("field");const auto*u=it.get("unique");
    require(c&&c->is_string()&&f&&f->is_string(),"CORRUPT catalog record");
    ix->collection=c->str;ix->field=f->str;if(const auto*n=it.get("name"))if(n->is_string())ix->name=n->str;ix->unique=u&&u->kind==json::Json::True;
    if(const auto*cl=it.get("columns")){
     require(cl->kind==json::Json::Array&&cl->items.size()>=2&&cl->items.size()<=8,"CORRUPT catalog index columns");
     for(const auto&x:cl->items){require(x.is_string(),"CORRUPT catalog index columns");ix->cols.push_back(x.str);ix->cpaths.push_back(json::split_path(x.str));}
    }else ix->path=json::split_path(ix->field);
    sec_.push_back(std::move(ix));
   }
   if(const auto*list=j.get("fulltext"))for(const auto&it:list->items){
    auto ix=std::make_unique<SecIndex>();ix->fulltext=true;
    const auto*c=it.get("collection");const auto*cl=it.get("columns");const auto*n=it.get("name");const auto*m=it.get("minLength");
    require(c&&c->is_string()&&cl&&cl->kind==json::Json::Array&&!cl->items.empty()&&cl->items.size()<=8&&n&&n->is_string(),"CORRUPT catalog fulltext index");
    ix->collection=c->str;ix->name=n->str;if(m&&m->is_number()&&m->num>=1&&m->num<=64)ix->min_len=size_t(m->num);
    for(const auto&x:cl->items){require(x.is_string(),"CORRUPT catalog fulltext index");ix->cols.push_back(x.str);ix->cpaths.push_back(json::split_path(x.str));ix->field+=(ix->field.empty()?"":",")+x.str;}
    sec_.push_back(std::move(ix));
   }
   if(const auto*t=j.get("tables"))for(const auto&it:t->items)if(it.is_string())tables_.push_back(it.str);
   if(const auto*sc=j.get("schemas"))for(const auto&[table,cols]:sc->fields){
    auto&out=columns_[table];
    for(const auto&c:cols.items){
     sql::ColumnDef d;const auto*n=c.get("name");const auto*t=c.get("type");const auto*nn=c.get("notNull");const auto*df=c.get("default");
     require(n&&n->is_string()&&t&&t->is_string(),"CORRUPT catalog column");
     d.name=n->str;d.type=t->str;d.not_null=nn&&nn->kind==json::Json::True;if(df){d.has_default=true;d.def=df->scalar();}
     if(const auto*len=c.get("length"))if(len->is_number()&&len->num>=1)d.length=size_t(len->num);
     out.push_back(std::move(d));
    }
   }
   if(const auto*ru=j.get("rules"))for(const auto&[table,v]:ru->fields)rules_[table]=compile_rules(table,rules_of(v));
  }
  cat_loaded_=true;
 }
 std::string catalog_json(const SecIndex*extra,const SecIndex*without)const{
  std::string s="{\"indexes\":[";bool first=true;
  auto add=[&](const SecIndex&ix){if(!first)s+=',';first=false;s+="{\"name\":"+json::quote(ix.name)+",\"collection\":"+json::quote(ix.collection)+",\"field\":"+json::quote(ix.field)+(ix.cols.empty()?"":",\"columns\":"+names_json(ix.cols))+",\"unique\":"+(ix.unique?"true":"false")+"}";};
  for(const auto&ix:sec_)if(ix.get()!=without&&!ix->fulltext)add(*ix);
  if(extra&&!extra->fulltext)add(*extra);
  // FULLTEXT indexes in their own list (a version without full-text search ignores it).
  s+="],\"fulltext\":[";first=true;
  auto addf=[&](const SecIndex&ix){if(!first)s+=',';first=false;s+="{\"name\":"+json::quote(ix.name)+",\"collection\":"+json::quote(ix.collection)+",\"columns\":"+names_json(ix.cols)+",\"minLength\":"+std::to_string(ix.min_len)+"}";};
  for(const auto&ix:sec_)if(ix.get()!=without&&ix->fulltext)addf(*ix);
  if(extra&&extra->fulltext)addf(*extra);
  s+="],\"tables\":[";for(size_t i=0;i<tables_.size();i++){if(i)s+=',';s+=json::quote(tables_[i]);}
  s+="],\"schemas\":{";bool firstt=true;
  for(const auto&[table,cols]:columns_){
   if(cols.empty())continue;
   if(!firstt)s+=',';firstt=false;s+=json::quote(table)+":[";
   for(size_t i=0;i<cols.size();i++){if(i)s+=',';s+=column_json(cols[i]);}
   s+="]";
  }
  s+="}";
  if(!rules_.empty())s+=",\"rules\":{"+all_rules_json()+"}";
  return s+"}";
 }
 // Writer lock held; lazy_ false.
 void ensure_ordered_locked(){
  if(ordered_built_)return;
  // Sorted in bounded RAM (runs merged from a temporary file when large); pages go to the
  // scratch file once the sets' memory budget is used.
  ExternalSorter sorter(path_+".sort",sort_budget());
  for(size_t s=0;s<shards_.size();s++)for_each_live(s,[&](std::string_view k,const Ref&,uint64_t){sorter.add(k);});
  SortedSet built;attach_set(built);size_t keep=options_.orderedMemory?options_.orderedMemory/2:SIZE_MAX;
  sorter.finish([&](std::string_view k){built.build_add(k,keep);});built.build_end();
  std::unique_lock<ReadWriteMutex>r(readers_);ordered_=std::move(built);ordered_built_=true;trim_sets_locked();
 }
 // One column of a composite entry (missing reads as null).
 static void encode_part(std::string&out,const json::Scalar&v){if(v.tag==json::MISSING)out+=char(json::NUL);else json::encode_into(out,v);}
 static std::string index_entry(const SecIndex&ix,std::string_view value,std::string_view id){
  if(!ix.cols.empty()){std::string e;for(const auto&p:ix.cpaths)encode_part(e,json::field(value,p));e.append(id.data(),id.size());return e;}
  auto e=json::encode(json::field(value,ix.path));e.append(id.data(),id.size());return e;
 }
 static bool unique_value(std::string_view enc){return !enc.empty()&&uint8_t(enc[0])!=json::MISSING&&uint8_t(enc[0])!=json::NUL;}
 // Length of the encoded values at the start of an entry of ix (the row key follows).
 static size_t values_length(const SecIndex&ix,std::string_view e){
  if(ix.cols.empty())return json::encoded_length(e);
  size_t n=0;for(size_t j=0;j<ix.cols.size();j++)n+=json::encoded_length(e.substr(n));return n;
 }
 // Whether a unique index checks these values: a composite one only when no column is NULL.
 static bool unique_values(const SecIndex&ix,std::string_view enc){
  if(ix.cols.empty())return unique_value(enc);
  for(size_t n=0;n<enc.size();n+=json::encoded_length(enc.substr(n)))if(!unique_value(enc.substr(n)))return false;
  return !enc.empty();
 }
 // Builds ix from the current state. Fails with CONFLICT if a unique index finds a duplicate.
 void build_index_locked(SecIndex&ix){
  if(ix.built)return;
  ensure_ordered_locked();
  std::string prefix=ix.collection+'\0';auto end=json::prefix_end(prefix);
  ExternalSorter sorter(path_+".sort",sort_budget());
  std::vector<std::string> fte;uint64_t docs=0,tokens=0;
  for(auto p=ordered_.seek(prefix);ordered_.valid(p);p=ordered_.next(p)){
   auto id=ordered_.get(p);if(id>=end)break;
   Ref r;auto h=index_hash(key_,id);if(!lookup(shard_of(h),id,h,r))continue;
   if(ix.fulltext){fulltext_entries(ix,load_ref(r).value,id,fte);if(!fte.empty()){docs++;tokens+=marker_tokens(fte[0]);}for(const auto&e:fte)sorter.add(e);}
   else sorter.add(index_entry(ix,load_ref(r).value,id));
  }
  SortedSet built;attach_set(built);size_t keep=options_.orderedMemory?options_.orderedMemory/2:SIZE_MAX;
  std::string prev;bool have=false;
  sorter.finish([&](std::string_view b){
   if(ix.unique&&have){auto a=std::string_view(prev);auto n=values_length(ix,a);
    if(unique_values(ix,a.substr(0,n))&&b.substr(0,values_length(ix,b))==a.substr(0,n))
     throw std::runtime_error("CONFLICT unique index "+ix.collection+"."+ix.field+" has duplicate values");}
   if(ix.unique){prev.assign(b);have=true;}
   built.build_add(b,keep);});
  built.build_end();
  std::unique_lock<ReadWriteMutex>r(readers_);ix.set=std::move(built);ix.built=true;ix.ft_docs=docs;ix.ft_tokens=tokens;trim_sets_locked();
 }
 // ---------- Scan column cache ----------
 // Aggregate queries that would scan a whole table read the columns they need from here:
 // one array per (table, column path), slot-aligned, kept current by every commit
 // (apply_catalog). Built on first use from verified rows; capped in total size.
 struct ColumnStore {
  struct Col {std::vector<std::string> path;std::vector<json::Scalar> v;};
  std::vector<std::string> ids;  // slot -> row key ("" = free slot)
  std::unordered_map<std::string,uint32_t> slot_of;
  std::vector<uint32_t> free;
  std::vector<std::unique_ptr<Col>> cols;
  const Col*find(const std::vector<std::string>&p)const{for(const auto&c:cols)if(c->path==p)return c.get();return nullptr;}
  size_t cells()const{return ids.size()*(cols.size()+1);}
 };
 std::unordered_map<std::string,std::unique_ptr<ColumnStore>> colstores_;
 static constexpr size_t COLUMN_CELLS_MAX=size_t(32)<<20;
 // Writer lock held; lazy_ false; ordered keys built. Returns null when over budget.
 const ColumnStore*ensure_columns_locked(const std::string&table,const std::vector<std::vector<std::string>>&paths){
  auto it=colstores_.find(table);const ColumnStore*cs=it==colstores_.end()?nullptr:it->second.get();
  bool complete=cs!=nullptr;for(const auto&p:paths)if(!cs||!cs->find(p))complete=false;
  if(complete)return cs;
  std::vector<std::vector<std::string>> all=paths;
  if(cs)for(const auto&c:cs->cols)if(std::find(all.begin(),all.end(),c->path)==all.end())all.push_back(c->path);
  std::string prefix=table+'\0',end=json::prefix_end(prefix);
  size_t rows=0;for(auto p=ordered_.seek(prefix);ordered_.valid(p)&&ordered_.get(p)<end;p=ordered_.next(p))rows++;
  size_t others=0;for(const auto&[t,s]:colstores_)if(t!=table)others+=s->cells();
  if(others+rows*(all.size()+1)>COLUMN_CELLS_MAX)return nullptr;
  auto fresh=std::make_unique<ColumnStore>();
  for(const auto&p:all){auto c=std::make_unique<ColumnStore::Col>();c->path=p;c->v.reserve(rows);fresh->cols.push_back(std::move(c));}
  fresh->ids.reserve(rows);fresh->slot_of.reserve(rows);std::string buf;
  for(auto p=ordered_.seek(prefix);ordered_.valid(p);p=ordered_.next(p)){
   auto id=ordered_.get(p);if(id>=end)break;
   Ref r;auto h=index_hash(key_,id);if(!lookup(shard_of(h),id,h,r))continue;
   auto value=view_ref(r,buf);
   fresh->slot_of.emplace(std::string(id),uint32_t(fresh->ids.size()));fresh->ids.emplace_back(id);
   for(auto&c:fresh->cols)c->v.push_back(json::field(value,c->path));
  }
  std::unique_lock<ReadWriteMutex>rl(readers_);auto*out=fresh.get();colstores_[table]=std::move(fresh);return out;
 }
 // One row of cs set to value, or removed (erase).
 static void column_put(ColumnStore&cs,const std::string&key,std::string_view value,bool erase){
  auto f=cs.slot_of.find(key);
  if(erase){
   if(f==cs.slot_of.end())return;
   uint32_t s=f->second;cs.ids[s].clear();for(auto&c:cs.cols)c->v[s]=json::Scalar{};cs.free.push_back(s);cs.slot_of.erase(f);
  }else{
   uint32_t s;
   if(f!=cs.slot_of.end())s=f->second;
   else{
    if(!cs.free.empty()){s=cs.free.back();cs.free.pop_back();cs.ids[s]=key;}
    else{s=uint32_t(cs.ids.size());cs.ids.push_back(key);for(auto&c:cs.cols)c->v.emplace_back();}
    cs.slot_of.emplace(key,s);
   }
   for(auto&c:cs.cols)c->v[s]=json::field(value,c->path);
  }
 }
 void apply_columns(const std::vector<Op>&ops){
  for(const auto&o:ops){
   auto coll=collection_of(o.key);if(coll.empty())continue;
   auto it=colstores_.find(std::string(coll));if(it==colstores_.end())continue;
   column_put(*it->second,o.key,o.value,o.erase);
  }
 }
 // Whether a condition of where can use an index of table (the column cache then is not
 // worth it). Catalog loaded.
 bool index_narrows(const std::string&table,const std::vector<query::Cond>&where)const{
  for(const auto&c:where)if(c.op!=query::Cond::Ne&&c.op!=query::Cond::Exists)for(const auto&ix:sec_)if(ix->collection==table&&!ix->fulltext&&(ix->field==c.field||(!ix->cols.empty()&&ix->cols[0]==c.field)))return true;
  return false;
 }
 // ---------- Group-commit hooks ----------
 struct IndexDelta {SecIndex*ix;std::string remove,add;};
 struct UniqueGroup {std::unordered_map<std::string,std::string> added;std::unordered_set<std::string> removed;};
 // Index changes for one transaction's operations, checked against unique indexes (the
 // committed index plus earlier transactions of this group). Throws CONFLICT.
 // fulltext false: FULLTEXT indexes are left out (a transaction's statements: only unique
 // values are checked before COMMIT).
 template<class Current> void index_deltas(const std::vector<Op>&ops,Current&&current,std::vector<IndexDelta>&out,std::unordered_map<SecIndex*,UniqueGroup>&groups,bool fulltext=true){
  size_t first=out.size();
  // The built indexes of each collection, and their field values read in one pass over
  // each document (top-level fields), instead of one parse per index. Large statements
  // compute rows in parallel on the pool (read-only work); results keep row order.
  // One value per column: a composite index takes one per column, in its column order.
  struct Plan {std::vector<SecIndex*> ixs,fts;std::vector<std::string> names;std::vector<const std::vector<std::string>*> paths;bool flat=true;};
  std::deque<std::pair<std::string_view,Plan>> plans;std::vector<const Plan*> plan_of(ops.size(),nullptr);  // a deque: plan_of points into it
  for(size_t i=0;i<ops.size();i++){
   auto coll=collection_of(ops[i].key);if(!has_index(coll))continue;
   auto it=std::find_if(plans.begin(),plans.end(),[&](const auto&p){return p.first==coll;});
   if(it==plans.end()){
    Plan p;
    for(auto&x:sec_)if(x->built&&x->collection==coll){
     if(x->fulltext){if(fulltext)p.fts.push_back(x.get());continue;}
     p.ixs.push_back(x.get());if(x->cols.empty())p.paths.push_back(&x->path);else for(const auto&cp:x->cpaths)p.paths.push_back(&cp);}
    for(const auto*pp:p.paths){p.names.push_back((*pp)[0]);p.flat&=pp->size()==1;}
    plans.emplace_back(coll,std::move(p));it=plans.end()-1;
   }
   if(!it->second.ixs.empty()||!it->second.fts.empty())plan_of[i]=&it->second;
  }
  std::vector<std::vector<IndexDelta>> per(ops.size());
  auto row=[&](size_t i){
   const Plan*p=plan_of[i];if(!p)return;const auto&o=ops[i];
   std::vector<json::Scalar> oldv,newv;
   auto values=[&](std::string_view doc,std::vector<json::Scalar>&v){
    if(p->flat)json::top_fields(doc,p->names,v);
    else{v.clear();for(const auto*pp:p->paths)v.push_back(json::field(doc,*pp));}
   };
   auto old=current(o.key);
   if(!p->ixs.empty()){if(old)values(*old,oldv);if(!o.erase)values(o.value,newv);}
   // A FULLTEXT index: the entries the row loses and gains (both lists sorted).
   for(auto*fx:p->fts){
    std::vector<std::string> oe,ne;if(old)fulltext_entries(*fx,*old,o.key,oe);if(!o.erase)fulltext_entries(*fx,o.value,o.key,ne);
    for(size_t a=0,b=0;a<oe.size()||b<ne.size();){
     if(b==ne.size()||(a<oe.size()&&oe[a]<ne[b]))per[i].push_back({fx,std::move(oe[a++]),std::string()});
     else if(a==oe.size()||ne[b]<oe[a])per[i].push_back({fx,std::string(),std::move(ne[b++])});
     else{a++;b++;}
    }
   }
   for(size_t k=0,at=0;k<p->ixs.size();k++){
    std::string oe,ne;size_t n=p->ixs[k]->cols.size();
    if(!n){
     if(old){oe.reserve(24+o.key.size());json::encode_into(oe,oldv[at]);oe+=o.key;}
     if(!o.erase){ne.reserve(24+o.key.size());json::encode_into(ne,newv[at]);ne+=o.key;}
     at++;
    }else{
     if(old){for(size_t j=0;j<n;j++)encode_part(oe,oldv[at+j]);oe+=o.key;}
     if(!o.erase){for(size_t j=0;j<n;j++)encode_part(ne,newv[at+j]);ne+=o.key;}
     at+=n;
    }
    if(oe!=ne)per[i].push_back({p->ixs[k],std::move(oe),std::move(ne)});
   }
  };
  constexpr size_t STEP=32;size_t chunks=(ops.size()+STEP-1)/STEP;
  parallel_for(chunks,ops.size()>=256,[&](size_t c){for(size_t i=c*STEP;i<std::min(ops.size(),(c+1)*STEP);i++)row(i);});
  for(auto&v:per)for(auto&d:v)out.push_back(std::move(d));
  // Unique checks after all of this transaction's removals, so swapping values between
  // documents in one transaction works. An entry conflicts unless it is being removed.
  // Views into out[] entries, which stay put from here on.
  std::unordered_set<std::string_view> removing;
  for(size_t i=first;i<out.size();i++)if(out[i].ix->unique&&!out[i].remove.empty())removing.insert(out[i].remove);
  struct Add {const SecIndex*ix;std::string_view enc,id;};std::vector<Add> adding;  // this transaction, checked by sorting below
  // Each new unique value is looked up in its index; read-only, so large statements check
  // rows in parallel. adding collects them for the in-transaction duplicate check.
  for(size_t i=first;i<out.size();i++){
   const auto&d=out[i];if(!d.ix->unique||d.add.empty())continue;
   std::string_view add=d.add;auto n=values_length(*d.ix,add);auto enc=add.substr(0,n),id=add.substr(n);
   if(unique_values(*d.ix,enc))adding.push_back({d.ix,enc,id});
  }
  auto check=[&](const Add&x){
   auto&ix=*const_cast<SecIndex*>(x.ix);auto gi=groups.find(&ix);const UniqueGroup*g=gi==groups.end()?nullptr:&gi->second;
   auto conflict=[&]{throw std::runtime_error("CONFLICT unique index "+ix.collection+"."+ix.field+" already has this value");};
   auto gone=[&](std::string_view entry){return removing.count(entry)>0||(g&&g->removed.count(std::string(entry))>0);};
   if(g){auto it=g->added.find(std::string(x.enc));if(it!=g->added.end()&&it->second!=x.id&&!gone(std::string(x.enc)+it->second))conflict();}
   for(auto p2=ix.set.seek(x.enc);ix.set.valid(p2);p2=ix.set.next(p2)){
    auto e=ix.set.get(p2);if(e.compare(0,x.enc.size(),x.enc)!=0)break;
    if(e.substr(x.enc.size())!=x.id&&!gone(e))conflict();
   }
  };
  {constexpr size_t STEP=32;size_t chunks=(adding.size()+STEP-1)/STEP;
   parallel_for(chunks,adding.size()>=256,[&](size_t c){for(size_t i=c*STEP;i<std::min(adding.size(),(c+1)*STEP);i++)check(adding[i]);});}
  // Two rows of this transaction with the same unique value.
  if(adding.size()>1){
   std::sort(adding.begin(),adding.end(),[](const Add&x,const Add&y){return x.ix!=y.ix?x.ix<y.ix:x.enc<y.enc;});
   for(size_t i=1;i<adding.size();i++)if(adding[i].ix==adding[i-1].ix&&adding[i].enc==adding[i-1].enc&&adding[i].id!=adding[i-1].id)
    throw std::runtime_error("CONFLICT unique index "+adding[i].ix->collection+"."+adding[i].ix->field+" already has this value");
  }
 }
 static void merge_unique(const std::vector<IndexDelta>&deltas,const std::vector<Op>&ops,std::unordered_map<SecIndex*,UniqueGroup>&groups){
  (void)ops;
  for(const auto&d:deltas){
   if(!d.ix->unique)continue;auto&g=groups[d.ix];
   if(!d.remove.empty()){g.removed.insert(d.remove);auto n=values_length(*d.ix,d.remove);auto it=g.added.find(d.remove.substr(0,n));if(it!=g.added.end()&&it->second==d.remove.substr(n))g.added.erase(it);}
   if(!d.add.empty()){auto n=values_length(*d.ix,d.add);g.added[d.add.substr(0,n)]=d.add.substr(n);}
  }
 }
 // Applied with readers_ held exclusively, right after install().
 void apply_catalog(const std::vector<Op>&ops,const std::vector<IndexDelta>&deltas){
  // Removals first, then additions per set in ascending order (one forward pass each).
  // Every entry ends with its row key, so a removal never cancels an addition.
  // Each set (ordered keys, every index) is independent: large commits update them in
  // parallel on the pool.
  struct Work {SortedSet*set;std::vector<std::string_view> remove,add;};std::vector<Work> work;
  if(ordered_built_){Work w{&ordered_,{},{}};for(const auto&o:ops)(o.erase?w.remove:w.add).push_back(o.key);work.push_back(std::move(w));}
  for(const auto&d:deltas){
   auto it=std::find_if(work.begin(),work.end(),[&](const Work&w){return w.set==&d.ix->set;});
   if(it==work.end()){work.push_back({&d.ix->set,{},{}});it=work.end()-1;}
   if(!d.remove.empty())it->remove.push_back(d.remove);
   if(!d.add.empty())it->add.push_back(d.add);
   if(d.ix->fulltext)fulltext_count(*d.ix,d.remove,d.add);
  }
  size_t total=0;for(const auto&w:work)total+=w.add.size()+w.remove.size();
  parallel_for(work.size(),total>=512,[&](size_t i){
   auto&w=work[i];for(auto r:w.remove)w.set->erase(r);
   if(w.add.size()>1)std::sort(w.add.begin(),w.add.end());
   w.set->insert_sorted(w.add.begin(),w.add.end());
  });
  if(!work.empty())trim_sets_locked();
  if(!colstores_.empty())apply_columns(ops);
 }
 // current + delta for an atomic increment. Integers stay exact (64-bit, overflow is an
 // error); otherwise doubles. A missing value counts as 0.
 static std::string add_number(const std::optional<std::string>&current,std::string_view delta){
  auto num=[](std::string_view s,bool&integer,long long&i,double&d){
   while(!s.empty()&&(s.front()==' '||s.front()=='\n'||s.front()=='\t'||s.front()=='\r'))s.remove_prefix(1);
   while(!s.empty()&&(s.back()==' '||s.back()=='\n'||s.back()=='\t'||s.back()=='\r'))s.remove_suffix(1);
   auto sc=json::scalar_of(s);if(sc.tag!=json::NUMBER)return false;d=sc.num;
   integer=s.find_first_of(".eE")==std::string_view::npos;
   if(integer){std::string t(s);errno=0;char*end=nullptr;i=strtoll(t.c_str(),&end,10);if(errno||*end)integer=false;}
   return true;};
  bool ai=true,bi=false;long long a=0,b=0;double ad=0,bd=0;
  if(current)require(num(*current,ai,a,ad),"INPUT increment: the current value is not a number");
  require(num(delta,bi,b,bd),"INPUT increment amount must be a number");
  if(ai&&bi){long long r;
#if defined(_MSC_VER)
   if((b>0&&a>LLONG_MAX-b)||(b<0&&a<LLONG_MIN-b))throw std::runtime_error("LIMIT increment overflows 64-bit integer");r=a+b;
#else
   if(__builtin_add_overflow(a,b,&r))throw std::runtime_error("LIMIT increment overflows 64-bit integer");
#endif
   return std::to_string(r);}
  double r=ad+bd;require(std::isfinite(r),"LIMIT increment result is not finite");
  char buf[40];snprintf(buf,sizeof buf,"%.17g",r);return buf;
 }
 // Builds what a query needs (first builds without the writer lock, then whatever is left
 // with it, held only meanwhile).
 void prepare_query(const std::string&collection){if(tl_nested())return;prebuild(collection);WriterLock w(*this);prepare_locked(collection);}
 // write: for a write statement, which needs the ordered keys and the collection's unique
 // indexes but not its other indexes (commits keep only built ones up to date; a later
 // build reads the rows as they are then), so it does not wait for a first build of those.
 void prepare_locked(const std::string&collection,bool write=false){
  // While a group's data is being written, statements of the next group may not build
  // anything (an index built now would miss that group's rows): they run afterwards.
  if(tl_overlap()){bool build=lazy_||!cat_loaded_||!ordered_built_;for(auto&ix:sec_)if(ix->collection==collection&&!ix->built&&(!write||ix->unique))build=true;if(build)throw Deferred{};}
  {std::shared_lock<ReadWriteMutex>r(readers_);healthy();}
  materialize();load_catalog_locked();ensure_ordered_locked();
  for(auto&ix:sec_)if(ix->collection==collection&&(!write||ix->unique))build_index_locked(*ix);
 }
 // ---------- First builds without the writer lock ----------
 // The ordered keys, a collection's secondary indexes and a table's column cache are built
 // when a statement first needs them. Built with the writer lock held, they held up every
 // commit meanwhile (half a second over 50,000 rows). prebuild() builds them before the
 // statement takes the writer lock, the way online compaction rewrites the file:
 //  1. Writer lock, briefly: m0 = the commit point, and a snapshot registered at m0
 //     (store_mvcc.inc), so every commit from then on records the reference each key it
 //     changes had (undo records).
 //  2. No writer lock: the rows unchanged since m0 (their value lies before m0's end), taken
 //     shard by shard under the shard's shared lock (the ordered keys) or in chunks of the
 //     ordered keys under readers_ shared (a collection), then read, sorted and built
 //     outside every lock. Rows changed meanwhile are left out.
 //  3. Writer lock: the keys changed since m0 (from the undo records), each in turn: the
 //     entry its m0 value gave removed (step 2 may have taken it), the entry its current
 //     value gives added. Then, with readers_ exclusively, the structures are published,
 //     unless built meanwhile. The writer lock is held for the commits made during the build.
 // One build at a time (building_); a statement that needs what is being built waits for it
 // (the ordered keys are published first, on their own), one that does not goes on. Best
 // effort: when it cannot run (a compaction under way: compact_m_; the catalog changed
 // meanwhile; duplicate values for a unique index) the statement's locked build runs, as
 // before, and reports any error. Values are read outside the shard locks: compact_m_,
 // held throughout, keeps the file (and its mapped views) in place.
 std::mutex bm_;std::condition_variable bcv_;bool building_=false;
 // What a statement on coll still lacks: 0 nothing, 1 something it needs, 2 only indexes a
 // write does without (see prepare_locked). readers_ held.
 int missing(const std::string&coll,bool write,const std::vector<std::vector<std::string>>*cols,const std::vector<query::Cond>*where)const{
  if(lazy_||!cat_loaded_||!ordered_built_)return 1;
  int m=0;
  for(const auto&ix:sec_)if(ix->collection==coll&&!ix->built){if(!write||ix->unique)return 1;m=2;}
  if(cols&&!coll.empty()&&table_exists(coll)&&!(where&&index_narrows(coll,*where))){
   auto it=colstores_.find(coll);if(it==colstores_.end())return 1;
   for(const auto&p:*cols)if(!it->second->find(p))return 1;
  }
  return m;
 }
 // Before a statement on coll takes the writer lock (see above). cols, where: the column
 // cache an aggregate wants, unless an index serves where.
 void prebuild(const std::string&coll,bool write=false,const std::vector<std::vector<std::string>>*cols=nullptr,const std::vector<query::Cond>*where=nullptr){
  if(tl_writer_held()==this||tl_overlap()||tl_nested())return;  // the locked build (or a deferral) follows
  auto need=[&]{std::shared_lock<ReadWriteMutex>r(readers_);healthy();return missing(coll,write,cols,where);};
  {
   std::unique_lock<std::mutex>l(bm_);
   for(;;){int m=need();if(!m||(m==2&&building_))return;if(!building_)break;bcv_.wait(l);}
   building_=true;
  }
  struct Done{Store&s;~Done(){{std::lock_guard<std::mutex>l(s.bm_);s.building_=false;}s.bcv_.notify_all();}}done{*this};
  try{
   if(build_unlocked(coll,true,nullptr,nullptr)){{std::lock_guard<std::mutex>l(bm_);}bcv_.notify_all();}  // statements waiting for the ordered keys only
   if(!coll.empty())build_unlocked(coll,false,cols,where);
  }catch(...){}  // the statement's locked build runs and reports the error
 }
 // A write statement: its table, and when constraints involve it, every table they read
 // (enforce_rules prepares them all: their lookups use every index).
 void prebuild_write(const std::string&table){
  prebuild(table,true);
  std::vector<std::string> involved;
  {
   std::shared_lock<ReadWriteMutex>r(readers_);if(!cat_loaded_||rules_.empty())return;
   std::set<std::string> all;for(const auto&[t,rr]:rules_){all.insert(t);for(const auto&f:rr.c.fks)all.insert(f.table);}
   if(!all.count(table))return;
   for(const auto&t:all)if(table_exists(t))involved.push_back(t);
  }
  for(const auto&t:involved)prebuild(t);
 }
 // One first build: the ordered keys (ordered), or coll's unbuilt indexes and, with cols, its
 // column cache. Returns whether it published anything.
 bool build_unlocked(const std::string&coll,bool ordered,const std::vector<std::vector<std::string>>*cols,const std::vector<query::Cond>*where){
  std::unique_lock<std::mutex>nc(compact_m_,std::try_to_lock);if(!nc.owns_lock())return false;
  Snap snap;struct End{Store&s;Snap&n;~End(){s.end_snapshot(n);}}end{*this,snap};
  std::vector<SecIndex*> ixs;std::vector<std::unique_ptr<SecIndex>> defs;  // copies of the definitions, read outside the writer lock
  std::vector<std::vector<std::string>> paths;
  // 1. The commit point, and what is missing.
  {
   std::lock_guard<std::mutex>w(writer_);healthy();materialize();load_catalog_locked();
   if(ordered_built_==ordered)return false;  // the ordered keys: built already; the rest: needs them first
   if(!ordered){
    for(auto&ix:sec_)if(ix->collection==coll&&!ix->built&&!ix->fulltext){  // FULLTEXT: the locked build (its entries differ)
     ixs.push_back(ix.get());auto d=std::make_unique<SecIndex>();
     d->name=ix->name;d->collection=ix->collection;d->field=ix->field;d->path=ix->path;d->unique=ix->unique;d->cols=ix->cols;d->cpaths=ix->cpaths;defs.push_back(std::move(d));
    }
    if(cols&&table_exists(coll)&&!(where&&index_narrows(coll,*where))){
     auto it=colstores_.find(coll);const ColumnStore*cs=it==colstores_.end()?nullptr:it->second.get();
     bool complete=cs!=nullptr;for(const auto&p:*cols)if(!cs||!cs->find(p))complete=false;
     if(!complete){paths=*cols;if(cs)for(const auto&c:cs->cols)if(std::find(paths.begin(),paths.end(),c->path)==paths.end())paths.push_back(c->path);}
    }
    if(defs.empty()&&paths.empty())return false;
   }
   if(options_.orderedMemory&&!set_file_)set_file_=std::make_unique<PageFile>(path_+".sets");  // attach_set below only reads it
   snap.store=this;snap.end=meta_.end;snap.epoch=catalog_epoch_;register_snapshot(snap);
  }
  const uint64_t m0=snap.end;const std::string prefix=coll+'\0',stop=json::prefix_end(prefix);
  const size_t keep=options_.orderedMemory?options_.orderedMemory/2:SIZE_MAX;
  // 2. Without the writer lock: the rows unchanged since m0.
  SortedSet oset;std::vector<std::unique_ptr<SortedSet>> sets;std::vector<char> ok(defs.size(),1);std::unique_ptr<ColumnStore> fresh;
  std::string buf;
  if(ordered){
   ExternalSorter sorter(path_+".sorting",sort_budget());std::vector<std::string> ks;
   for(size_t s=0;s<shards_.size();s++){
    ks.clear();
    {std::shared_lock<ReadWriteMutex>l(shards_[s].mutex);healthy();for_each_live(s,[&](std::string_view k,const Ref&r,uint64_t){if(r.offset<m0)ks.emplace_back(k);});}
    for(const auto&k:ks)sorter.add(k);
   }
   attach_set(oset);sorter.finish([&](std::string_view k){oset.build_add(k,keep);});oset.build_end();
  }else{
   // The collection's keys in order, 4,096 at a time under readers_ shared (shard indexes
   // then change only under readers_ exclusively: lookups need no shard lock). A commit
   // waiting for readers_ goes first (the lock may prefer readers).
   auto walk=[&](auto&&f){
    std::string last;bool more=true,first=true;
    while(more){
     {
      std::shared_lock<ReadWriteMutex>r(readers_);healthy();
      auto p=ordered_.seek(first?prefix:last);if(!first&&ordered_.valid(p)&&ordered_.get(p)==last)p=ordered_.next(p);
      first=false;more=false;
      for(size_t n=0;ordered_.valid(p);p=ordered_.next(p)){auto k=ordered_.get(p);if(k>=stop)break;if(++n>4096){more=true;break;}last.assign(k);f(k);}
     }
     for(auto t=std::chrono::steady_clock::now();install_waiting_.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()-t<std::chrono::milliseconds(20);)std::this_thread::yield();
    }
   };
   if(!paths.empty()){  // the cache's budget, counted as ensure_columns_locked does
    size_t rows=0,others=0;walk([&](std::string_view){rows++;});
    {std::shared_lock<ReadWriteMutex>r(readers_);for(const auto&[t,cs]:colstores_)if(t!=coll)others+=cs->cells();}
    if(others+rows*(paths.size()+1)>COLUMN_CELLS_MAX){paths.clear();if(defs.empty())return false;}
   }
   std::vector<std::pair<std::string,Ref>> rows;
   walk([&](std::string_view k){Ref ref;auto h=index_hash(key_,k);if(lookup(shard_of(h),k,h,ref)&&ref.offset<m0)rows.emplace_back(std::string(k),ref);});
   // Rows are read and parsed in parallel on the pool (read-only work); index entries a block
   // at a time, for the sorter.
   constexpr size_t BLOCK=8192,STEP=256;std::vector<std::string> ents;
   for(size_t i=0;i<defs.size();i++){
    const SecIndex&d=*defs[i];ExternalSorter sorter(path_+".sorting",sort_budget());
    for(size_t at=0;at<rows.size();at+=BLOCK){
     size_t n=std::min(BLOCK,rows.size()-at);ents.resize(n);
     parallel_for((n+STEP-1)/STEP,n>=1024,[&](size_t c){std::string b;for(size_t j=c*STEP;j<std::min(n,(c+1)*STEP);j++)ents[j]=index_entry(d,view_ref(rows[at+j].second,b),rows[at+j].first);});
     for(const auto&e:ents)sorter.add(e);
    }
    auto set=std::make_unique<SortedSet>();attach_set(*set);std::string prev;bool have=false;
    sorter.finish([&](std::string_view b){
     if(!ok[i])return;
     if(d.unique&&have){auto a=std::string_view(prev);auto n=values_length(d,a);if(unique_values(d,a.substr(0,n))&&b.substr(0,values_length(d,b))==a.substr(0,n)){ok[i]=0;return;}}  // the locked build reports it
     if(d.unique){prev.assign(b);have=true;}
     set->build_add(b,keep);});
    set->build_end();sets.push_back(std::move(set));
   }
   if(!paths.empty()){
    fresh=std::make_unique<ColumnStore>();
    for(const auto&p:paths){auto c=std::make_unique<ColumnStore::Col>();c->path=p;fresh->cols.push_back(std::move(c));}
    for(auto&c:fresh->cols)c->v.resize(rows.size());
    parallel_for((rows.size()+STEP-1)/STEP,rows.size()>=1024,[&](size_t c){std::string b;for(size_t j=c*STEP;j<std::min(rows.size(),(c+1)*STEP);j++){auto value=view_ref(rows[j].second,b);for(auto&col:fresh->cols)col->v[j]=json::field(value,col->path);}});
    fresh->ids.reserve(rows.size());fresh->slot_of.reserve(rows.size());
    for(const auto&[k,r]:rows){fresh->slot_of.emplace(k,uint32_t(fresh->ids.size()));fresh->ids.push_back(k);}
   }
  }
  // 3. With the writer lock: the keys changed since m0, then publication.
  std::lock_guard<std::mutex>w(writer_);healthy();
  if(catalog_epoch_!=snap.epoch)return false;  // an index dropped, the catalog reloaded: the locked build
  std::vector<std::pair<std::string,UndoRec>> changed;  // each with its first change since m0, which holds its m0 reference
  {
   std::lock_guard<std::mutex>l(undo_m_);std::unordered_set<std::string_view> seen;
   for(const auto&[at,key]:undo_log_)if(at>=m0&&(ordered||(key->size()>prefix.size()&&!key->compare(0,prefix.size(),prefix)))&&seen.insert(*key).second){
    const UndoRec*u=undo_at(*key,m0);if(!u)return false;  // kept while the snapshot is registered: never missing
    changed.push_back({*key,*u});
   }
  }
  auto live=[&](const std::string&k,Ref&r){auto h=index_hash(key_,k);return lookup(shard_of(h),k,h,r);};
  if(ordered){
   if(ordered_built_)return false;
   for(const auto&[k,u]:changed){oset.erase(k);Ref r;if(live(k,r))oset.insert(k);}
  }else{
   for(size_t i=0;i<defs.size();i++){
    // A commit to the collection builds its unique indexes first (commit_group_locked).
    if(!ok[i]||ixs[i]->built||(defs[i]->unique&&!changed.empty())){ok[i]=0;continue;}
    for(const auto&[k,u]:changed){
     if(u.existed)sets[i]->erase(index_entry(*defs[i],view_ref(u.prev,buf),k));
     Ref r;if(live(k,r))sets[i]->insert(index_entry(*defs[i],view_ref(r,buf),k));
    }
   }
   if(fresh){
    auto it=colstores_.find(coll);bool complete=it!=colstores_.end();
    if(complete)for(const auto&p:paths)if(!it->second->find(p))complete=false;
    if(complete)fresh.reset();
    else{
     for(const auto&[k,u]:changed){Ref r;if(live(k,r))column_put(*fresh,k,view_ref(r,buf),false);else column_put(*fresh,k,std::string_view(),true);}
     size_t others=0;for(const auto&[t,cs]:colstores_)if(t!=coll)others+=cs->cells();
     if(others+fresh->cells()>COLUMN_CELLS_MAX)fresh.reset();
    }
   }
  }
  std::unique_lock<ReadWriteMutex>r(readers_);bool any=false;
  if(ordered){ordered_=std::move(oset);ordered_built_=true;any=true;}
  for(size_t i=0;i<defs.size();i++)if(ok[i]){ixs[i]->set=std::move(*sets[i]);ixs[i]->built=true;any=true;}
  if(fresh){colstores_[coll]=std::move(fresh);any=true;}
  if(any)trim_sets_locked();
  return any;
 }
public:
 struct QueryItem {std::string key;uint64_t version=0;std::string value;};
 struct QueryResult {std::vector<QueryItem> items;std::string next;uint64_t count=0;bool counted=false;};
 struct IndexInfo {std::string name,collection,field;bool unique=false,built=false;size_t entries=0;};
 // Runs a query (query::parse) against one consistent committed state.
 QueryResult query(const query::Spec&q){
  prepare_query(q.collection);
  std::shared_lock<ReadWriteMutex>r(readers_);healthy();
  return query_core(q);
 }
 // The query itself. Callers hold readers_ shared, or the writer lock after prepare_locked.
 // The parts of two sorted lists of [lo, hi) ranges that both cover ("" as hi: no bound).
 static query::Ranges intersect_ranges(const query::Ranges&a,const query::Ranges&b){
  query::Ranges out;
  for(const auto&x:a)for(const auto&y:b){
   std::string lo=std::max(x.first,y.first);
   std::string hi=x.second.empty()?y.second:y.second.empty()?x.second:std::min(x.second,y.second);
   if(hi.empty()||lo<hi)out.push_back({std::move(lo),std::move(hi)});
  }
  std::sort(out.begin(),out.end());
  if(out.empty())out.push_back({std::string(1,'\x01'),std::string(1,'\x01')});  // matches nothing
  return out;
 }
 // The best composite index for q: = or IN on its leading columns, then a range on the next
 // one (3 per column fixed, 2 for the range: the scores of one-column indexes). ordered: its
 // entries come in the requested order once the WHERE fixes (one value each) the columns
 // before the ORDER BY ones. used: the conditions its ranges decide exactly.
 struct MultiPlan {SecIndex*ix=nullptr;query::Ranges ranges;std::vector<char> used;int score=0;bool ordered=false;};
 MultiPlan composite_plan(const query::Spec&q){
  using C=query::Cond;MultiPlan best;
  for(auto&xp:sec_){
   auto&ix=*xp;if(ix.cols.empty()||ix.fulltext||!ix.built||ix.collection!=q.collection)continue;
   MultiPlan m;m.ix=&ix;m.used.assign(q.where.size(),0);
   std::vector<std::string> pre{std::string()};  // encoded values of the fixed columns, sorted
   std::vector<char> one(ix.cols.size(),0);  // fixed to one value
   size_t j=0;
   for(;j<ix.cols.size();j++){
    int pick=-1;
    for(size_t i=0;i<q.where.size();i++){
     const auto&c=q.where[i];if(c.is_key||c.field!=ix.cols[j])continue;
     if(c.op==C::Eq&&!sql::is_null(c.v)){pick=int(i);break;}
     // Not IN (NULL): the index does not tell NULL from missing.
     if(c.op==C::In&&pick<0&&std::none_of(c.list.begin(),c.list.end(),[](const json::Scalar&x){return sql::is_null(x);}))pick=int(i);
    }
    if(pick<0)break;
    const auto&c=q.where[size_t(pick)];std::vector<std::string> vals;
    if(c.op==C::Eq)vals.push_back(json::encode(c.v));
    else{for(const auto&x:c.list)vals.push_back(json::encode(x));std::sort(vals.begin(),vals.end());vals.erase(std::unique(vals.begin(),vals.end()),vals.end());}
    if(pre.size()*vals.size()>1000)break;  // too many ranges: the rest is checked per row
    std::vector<std::string> next;next.reserve(pre.size()*vals.size());
    for(const auto&p:pre)for(const auto&v:vals)next.push_back(p+v);  // still sorted: encodings are self-delimiting
    pre=std::move(next);one[j]=vals.size()==1;m.used[size_t(pick)]=1;
   }
   m.score=3*int(j);
   query::Ranges tail;
   if(j<ix.cols.size())for(size_t i=0;i<q.where.size();i++){
    const auto&c=q.where[i];
    if(c.is_key||c.field!=ix.cols[j]||!(c.op==C::Gt||c.op==C::Gte||c.op==C::Lt||c.op==C::Lte||c.op==C::Prefix)||sql::is_null(c.v))continue;
    tail=tail.empty()?query::ranges_for(c):intersect_ranges(tail,query::ranges_for(c));m.used[i]=1;
   }
   if(!tail.empty())m.score+=2;
   for(const auto&p:pre){
    if(tail.empty())m.ranges.push_back({p,json::prefix_end(p)});
    else for(const auto&t:tail)m.ranges.push_back({p+t.first,t.second.empty()?json::prefix_end(p):p+t.second});
   }
   // The order: each index column in turn is the next ORDER BY column or fixed to one value.
   if(!q.order_key&&!q.count){
    std::vector<const std::string*> order{&q.order_field};for(const auto&f:q.order_more)order.push_back(&f);
    auto constant=[&](const std::string&f){for(size_t x=0;x<j;x++)if(one[x]&&ix.cols[x]==f)return true;return false;};
    size_t oi=0,k=0;
    for(;;){
     while(oi<order.size()&&constant(*order[oi]))oi++;
     if(oi==order.size()||k==ix.cols.size())break;
     if(ix.cols[k]==*order[oi])oi++;else if(!one[k])break;
     k++;
    }
    bool ok=oi==order.size();
    // Ties then go by the row key only when no further column varies.
    if(ok&&q.order_more_key)for(;k<ix.cols.size();k++)if(!one[k])ok=false;
    m.ordered=ok;
   }
   if(!best.ix||m.score>best.score||(m.score==best.score&&m.ordered&&!best.ordered))best=std::move(m);
  }
  return best;
 }
 QueryResult query_core(const query::Spec&q){
  const std::string prefix=q.collection+'\0';
  // Source: the best index for a condition, else an index on the sort field, else the
  // collection's keys in order (narrowed by conditions on _key).
  SecIndex*src=nullptr;query::Ranges ranges;int best=0;
  std::optional<MultiPlan> multi,order_only;bool multi_order=false;
  if(q.keys){
   // Rows a FULLTEXT index found (SQL MATCH): exactly these, in key order.
   std::string lo=prefix,hi=json::prefix_end(prefix);
   for(const auto&c:q.where){std::string l,h;if(c.is_key&&query::key_range(c,prefix,l,h)){lo=std::max(lo,l);hi=std::min(hi,h);}}
   ranges.reserve(q.keys->size());
   for(const auto&k:*q.keys)if(k>=lo&&k<hi)ranges.push_back({k,k+'\0'});
   if(ranges.empty())ranges.push_back({prefix,prefix});  // matches nothing
  }else{
  for(const auto&c:q.where){
   if(c.is_key)continue;
   int score=c.op==query::Cond::Eq||c.op==query::Cond::In?3:c.op==query::Cond::Ne||c.op==query::Cond::Exists?0:2;
   if(score<=best)continue;
   for(auto&ix:sec_)if(ix->built&&ix->collection==q.collection&&ix->field==c.field&&ix->cols.empty()){src=ix.get();ranges=query::ranges_for(c);best=score;break;}
  }
  // A composite index wins when it narrows more, or as much and also yields the order.
  for(const auto&ix:sec_)if(!ix->cols.empty()&&!ix->fulltext&&ix->built&&ix->collection==q.collection){
   auto m=composite_plan(q);
   if(m.score>best||(m.score==best&&m.score>0&&m.ordered)){src=m.ix;ranges=m.ranges;best=m.score;multi_order=m.ordered;multi=std::move(m);}
   else if(m.ordered&&!src)order_only=std::move(m);  // for the order alone, unless a one-column index gives it (below)
   break;
  }
  // A small LIMIT ordered by another indexed field: when the condition matches many rows,
  // walking the sort index in order and filtering stops early instead of sorting them all.
  if(src&&!q.order_key&&!q.count&&!q.unlimited&&!q.sink&&src->field!=q.order_field&&q.order_more.empty()&&!multi_order){
   for(auto&ix:sec_)if(ix->built&&ix->collection==q.collection&&ix->field==q.order_field&&ix->cols.empty()){
    size_t cap=q.limit*16+64,n=0;
    for(const auto&rg:ranges){for(auto p=src->set.seek(rg.first);src->set.valid(p)&&n<=cap;p=src->set.next(p)){if(!rg.second.empty()&&src->set.get(p)>=rg.second)break;n++;}if(n>cap)break;}
    if(n>cap){src=ix.get();ranges={{std::string(),std::string()}};multi.reset();}
    break;
   }
  }
  if(!src&&!q.order_key&&q.order_more.empty())for(auto&ix:sec_)if(ix->built&&ix->collection==q.collection&&ix->field==q.order_field&&ix->cols.empty()){src=ix.get();ranges={{std::string(),std::string()}};break;}
  if(!src&&order_only){src=order_only->ix;ranges=order_only->ranges;multi_order=true;multi=std::move(order_only);}  // the whole index, in order
  if(!src){
   std::string lo=prefix,hi=json::prefix_end(prefix);
   for(const auto&c:q.where){std::string l,h;if(c.is_key&&query::key_range(c,prefix,l,h)){lo=std::max(lo,l);hi=std::min(hi,h);}}
   ranges={{lo,std::max(lo,hi)}};
  }
  }
  // Every condition on the source index's own field narrows its ranges, which stay inside
  // the value's type class and so decide these operators exactly: such a condition is not
  // checked again against the row, and when no other one needs it the row is not read
  // (age BETWEEN 20 AND 40 scans [20, 40], not [20, end), and counts from the index alone).
  std::vector<char> covered(q.where.size(),0);
  if(multi)covered=multi->used;
  else if(src){
   using C=query::Cond;
   bool whole=ranges.size()==1&&ranges[0].first.empty()&&ranges[0].second.empty();  // picked for the order only
   for(size_t i=0;i<q.where.size();i++){
    const auto&c=q.where[i];
    if(c.is_key||c.field!=src->field||!(c.op==C::Eq||c.op==C::In||c.op==C::Gt||c.op==C::Gte||c.op==C::Lt||c.op==C::Lte||c.op==C::Prefix))continue;
    if(c.op!=C::In&&sql::is_null(c.v))continue;
    ranges=whole?query::ranges_for(c):intersect_ranges(ranges,query::ranges_for(c));whole=false;covered[i]=1;
   }
  }
  const SortedSet&set=src?src->set:ordered_;
  // Streaming: the source already yields the requested order, so the scan stops at the limit.
  bool streaming=!q.count&&(multi?multi_order:src?(!q.order_key&&src->field==q.order_field&&q.order_more.empty()):q.order_key);
  bool other_conds=false;for(size_t i=0;i<q.where.size();i++)other_conds|=!q.where[i].is_key&&!covered[i];
  bool need_value=(q.count?false:!q.keys_only)||bool(q.sink)||bool(q.residual)||other_conds||(!q.order_key&&!streaming);
  QueryResult res;
  // The statement's snapshot (store_mvcc.inc): only the outermost scan lets commits through.
  Snap*sn=tl_snap();if(sn&&sn->store!=this)sn=nullptr;
  ScanDepth depth(sn);
  const bool can_yield=sn&&sn->lock&&sn->depth==1;
  if(q.count&&q.where.empty()&&!q.residual&&!src&&!q.keys&&!(sn&&sn->registered)){
   for(const auto&rg:ranges)for(auto p=set.seek(rg.first);set.valid(p);p=set.next(p)){if(!rg.second.empty()&&set.get(p)>=rg.second)break;res.count++;}
   res.counted=true;return res;
  }
  struct Hit {std::string sort;QueryItem item;};std::vector<Hit> hits;size_t bytes=0;bool more=false;
  std::string vbuf,stop;bool stopped=false;  // stop: where a streaming scan ended (a serializable transaction's read set)
  // known: the entry's reference when the caller already has it (unordered full scans);
  // past: a row's version from the snapshot's undo records.
  auto visit=[&](std::string_view entry,const Ref*known=nullptr,bool past=false)->bool{
   check_deadline();
   std::string_view id=src?entry.substr(src->cols.empty()?json::encoded_length(entry):values_length(*src,entry)):entry;
   if(id.size()<=prefix.size()||id.compare(0,prefix.size(),prefix)!=0)return true;
   if(streaming&&q.has_after&&(q.desc?entry>=q.after:entry<=q.after))return true;
   Ref ref;if(known)ref=*known;else{auto h=index_hash(key_,id);if(!lookup(shard_of(h),id,h,ref))return true;}
   if(!past&&sn&&sn->registered&&ref.offset>=sn->end)return true;  // changed after the snapshot: its version comes from the undo records
   std::string_view value;if(need_value)value=view_ref(ref,vbuf);
   std::string_view user=id.substr(prefix.size());
   for(size_t i=0;i<q.where.size();i++){
    if(covered[i])continue;  // decided by the index range
    const auto&c=q.where[i];
    json::Scalar d;if(c.is_key){d.tag=json::STRING;d.str.assign(user);}else d=json::field(value,c.path);
    if(!query::match(c,d))return true;
   }
   if(q.residual&&!q.residual(user,value))return true;
   if(q.count){res.count++;return true;}
   if(q.sink){q.sink(user,ref.version,value);return true;}
   std::string sort;
   if(streaming)sort.assign(entry);
   else if(q.order_key)sort.assign(id);
   else if(q.order_more.empty()){sort=json::encode(json::field(value,q.order_path));sort.append(id);}
   else{encode_part(sort,json::field(value,q.order_path));for(const auto&p:q.order_more_paths)encode_part(sort,json::field(value,p));sort.append(id);}
   if(!streaming&&q.has_after&&(q.desc?sort>=q.after:sort<=q.after))return true;
   if(streaming&&((!q.unlimited&&hits.size()>=q.limit)||bytes>=(256u<<20))){more=true;if(sn&&sn->reads){stop.assign(entry);stopped=true;}return false;}
   bytes+=value.size()+user.size();
   hits.push_back({std::move(sort),{std::string(user),ref.version,q.keys_only?std::string():std::string(value)}});
   return true;
  };
  // A whole-collection scan whose order does not matter (counts, aggregates, results sorted
  // afterwards) walks the in-memory index directly instead of looking up each key.
  bool whole=!src&&ranges.size()==1&&ranges[0].first==prefix&&ranges[0].second==json::prefix_end(prefix);
  const bool shard_mode=whole&&!streaming&&!lazy_;
  // Rows changed since the snapshot, as of it: emitted where they belong in the scan order.
  // Created only for a scan under a registered snapshot (or at its first yield): a short
  // query allocates nothing for it.
  std::optional<SnapScan> ss;
  auto snapscan=[&]()->SnapScan&{if(!ss)ss.emplace(SnapScan{*this,sn,prefix,src,ranges,q.desc,shard_mode});return *ss;};
  if(sn&&sn->registered)snapscan().absorb();
  auto has_pending=[&]{return ss&&ss->next<ss->pend.size();};
  auto emit_pending=[&](auto&&before)->bool{
   while(has_pending()&&before(std::string_view(ss->pend[ss->next].entry))){auto&p=ss->pend[ss->next++];if(!visit(p.entry,&p.ref,true))return false;}
   return true;
  };
  auto pending_upto=[&](std::string_view e){return emit_pending([&](std::string_view x){return q.desc?x>=e:x<=e;});};
  if(shard_mode){
   for(size_t s=0;s<shards_.size();s++){
    for_each_live(s,[&](std::string_view k,const Ref&r,uint64_t){visit(k,&r);});
    if(can_yield&&want_yield(*sn)){auto&S=snapscan();S.shard_now=s+1;yield_snapshot(*sn);S.absorb();}
   }
   if(has_pending())emit_pending([](std::string_view){return true;});
  }else if(!q.desc){
   for(const auto&rg:ranges){
    std::string lo=rg.first;if(streaming&&q.has_after&&q.after>lo)lo=q.after;
    bool go=true;
    for(auto p=set.seek(lo);set.valid(p);){
     auto e=set.get(p);if(!rg.second.empty()&&e>=rg.second)break;
     if(has_pending()&&!(go=pending_upto(e)))break;
     if(!(go=visit(e)))break;
     if(can_yield&&want_yield(*sn)){
      // Positions in the set do not survive the commit: found again by value.
      auto&S=snapscan();S.last.assign(e);S.have_last=true;yield_snapshot(*sn);S.absorb();
      p=set.seek(S.last);if(set.valid(p)&&set.get(p)==S.last)p=set.next(p);
      continue;
     }
     p=set.next(p);
    }
    if(go&&has_pending())go=emit_pending([&](std::string_view x){return rg.second.empty()||x<rg.second;});
    if(!go)break;
   }
  }else{
   for(auto it=ranges.rbegin();it!=ranges.rend();++it){
    std::string hi=it->second;if(streaming&&q.has_after&&(hi.empty()||q.after<hi))hi=q.after;
    bool go=true;
    for(auto p=hi.empty()?set.prev(set.end()):set.prev(set.seek(hi));set.valid(p);){
     auto e=set.get(p);if(e<it->first)break;
     if(has_pending()&&!(go=pending_upto(e)))break;
     if(!(go=visit(e)))break;
     if(can_yield&&want_yield(*sn)){
      auto&S=snapscan();S.last.assign(e);S.have_last=true;yield_snapshot(*sn);S.absorb();
      p=set.prev(set.seek(S.last));
      continue;
     }
     p=set.prev(p);
    }
    if(go&&has_pending())go=emit_pending([&](std::string_view x){return x>=it->first;});
    if(!go)break;
   }
  }
  // A serializable transaction's scan: the ranges it walked (up to where it stopped).
  // A MATCH visits only the rows its words found; a row added later with those words is
  // not among them, so a MATCH counts as reading the whole table.
  if(sn&&sn->reads)note_reads(*sn->reads,q.collection,src,stopped?intersect_ranges(ranges,q.desc?query::Ranges{{stop,std::string()}}:query::Ranges{{std::string(),stop+'\0'}}):ranges,q.keys||(whole&&!stopped));
  if(q.count){res.counted=true;return res;}
  if(!streaming){
   auto less=[&](const Hit&a,const Hit&b){return q.desc?a.sort>b.sort:a.sort<b.sort;};
   if(!q.unlimited&&hits.size()>q.limit){std::partial_sort(hits.begin(),hits.begin()+std::ptrdiff_t(q.limit),hits.end(),less);more=true;hits.resize(q.limit);}
   else std::sort(hits.begin(),hits.end(),less);
  }
  if(more&&!hits.empty())res.next=query::hex(hits.back().sort);
  res.items.reserve(hits.size());for(auto&h:hits)res.items.push_back(std::move(h.item));
  return res;
 }
 QueryResult query(std::string_view spec){return query(query::parse(spec));}
 // Secondary index on a JSON field (dotted path). unique: no two documents may share a
 // value (null and missing excepted); creating one over duplicates fails with CONFLICT.
 void create_index(const std::string&collection,const std::string&field,bool unique=false,const std::string&name=""){
  char tok;GateHold gate(*this,&tok);std::lock_guard<std::mutex>w(writer_);create_index_locked(collection,field,unique,name);
 }
 // Composite index on several fields, in order (see create_composite_locked).
 void create_index(const std::string&collection,const std::vector<std::string>&fields,bool unique=false,const std::string&name=""){
  char tok;GateHold gate(*this,&tok);std::lock_guard<std::mutex>w(writer_);create_composite_locked(collection,fields,unique,name);
 }
 // Returns false when an identical index already exists.
 bool create_index_locked(const std::string&collection,const std::string&field,bool unique,const std::string&name){
  require(query::valid_collection(collection),"INPUT index collection");auto path=json::split_path(field);require(field!="_key"&&field!="id","INPUT the key is always indexed");
  {std::shared_lock<ReadWriteMutex>r(readers_);healthy();}
  materialize();load_catalog_locked();
  for(const auto&ix:sec_){
   if(ix->collection==collection&&ix->field==field&&!ix->fulltext){require(ix->unique==unique,"INPUT an index on "+collection+"."+field+" exists with a different UNIQUE setting; drop it first");return false;}
   require(name.empty()||ix->name!=name||ix->collection!=collection,"INPUT index name "+name+" is already used");
  }
  require(sec_.size()<256,"LIMIT at most 256 indexes");
  auto ix=std::make_unique<SecIndex>();ix->name=name.empty()?collection+"_"+field+"_idx":name;ix->collection=collection;ix->field=field;ix->path=std::move(path);ix->unique=unique;
  build_index_locked(*ix);
  write_catalog_locked(ix.get(),nullptr);
  std::unique_lock<ReadWriteMutex>r(readers_);sec_.push_back(std::move(ix));return true;
 }
 // An index over 1 to 8 columns (one column: create_index_locked). Returns false when an
 // index on the same columns, in the same order, already exists.
 bool create_composite_locked(const std::string&collection,const std::vector<std::string>&cols,bool unique,const std::string&name){
  if(cols.size()==1)return create_index_locked(collection,cols[0],unique,name);
  require(query::valid_collection(collection),"INPUT index collection");
  require(cols.size()>=2&&cols.size()<=8,"LIMIT an index covers at most 8 columns");
  std::string field;std::vector<std::vector<std::string>> paths;
  for(size_t i=0;i<cols.size();i++){
   require(cols[i]!="_key"&&cols[i]!="id","INPUT the key is always indexed (leave id out of the column list)");
   for(size_t k=0;k<i;k++)require(cols[k]!=cols[i],"INPUT the index lists "+cols[i]+" twice");
   paths.push_back(json::split_path(cols[i]));field+=(i?",":"")+cols[i];
  }
  {std::shared_lock<ReadWriteMutex>r(readers_);healthy();}
  materialize();load_catalog_locked();
  for(const auto&ix:sec_){
   if(ix->collection==collection&&ix->cols==cols&&!ix->fulltext){require(ix->unique==unique,"INPUT an index on "+collection+" ("+field+") exists with a different UNIQUE setting; drop it first");return false;}
   require(name.empty()||ix->name!=name||ix->collection!=collection,"INPUT index name "+name+" is already used");
  }
  require(sec_.size()<256,"LIMIT at most 256 indexes");
  auto ix=std::make_unique<SecIndex>();ix->name=name.empty()?default_index_name(collection,cols):name;ix->collection=collection;ix->field=field;ix->cols=cols;ix->cpaths=std::move(paths);ix->unique=unique;
  build_index_locked(*ix);
  write_catalog_locked(ix.get(),nullptr);
  std::unique_lock<ReadWriteMutex>r(readers_);sec_.push_back(std::move(ix));return true;
 }
 static std::string default_index_name(const std::string&collection,const std::vector<std::string>&cols){std::string n=collection;for(const auto&c:cols)n+="_"+c;return n+"_idx";}
 // An index's columns (one for a one-column index).
 static std::vector<std::string> index_columns(const SecIndex&ix){return ix.cols.empty()?std::vector<std::string>{ix.field}:ix.cols;}
 bool drop_index(const std::string&collection,const std::string&field){
  char tok;GateHold gate(*this,&tok);std::lock_guard<std::mutex>w(writer_);return drop_index_locked(collection,field);
 }
 // field may also be the index name.
 bool drop_index_locked(const std::string&collection,const std::string&field){
  {std::shared_lock<ReadWriteMutex>r(readers_);healthy();}
  materialize();load_catalog_locked();
  auto it=std::find_if(sec_.begin(),sec_.end(),[&](const auto&ix){return (collection.empty()||ix->collection==collection)&&(ix->field==field||ix->name==field);});
  if(it==sec_.end())return false;
  write_catalog_locked(nullptr,it->get());
  std::unique_lock<ReadWriteMutex>r(readers_);catalog_epoch_++;sec_.erase(it);return true;
 }
 void write_catalog_locked(const SecIndex*extra,const SecIndex*without){
  std::vector<Op> ops(1);ops[0].key.assign(catalog_key());ops[0].value=catalog_json(extra,without);
  auto out=commit_group_locked({&ops},true);if(out[0].error)std::rethrow_exception(out[0].error);
 }
 std::vector<IndexInfo> indexes(){
  {std::lock_guard<std::mutex>w(writer_);materialize();load_catalog_locked();}
  std::shared_lock<ReadWriteMutex>r(readers_);std::vector<IndexInfo> out;
  for(const auto&ix:sec_)out.push_back({ix->name,ix->collection,ix->field,ix->unique,ix->built,ix->set.size()});
  return out;
 }
