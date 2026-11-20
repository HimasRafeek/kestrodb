// Included inside class Store (kestro.hpp): CHECK, FOREIGN KEY and multi-column UNIQUE
// constraints of SQL tables. They live in the catalog record and are enforced at the end
// of every SQL INSERT, UPDATE and DELETE, against the rows as the statement leaves them
// (the transaction's earlier changes included). See SQL.md.
//
// Writes are serialized (one writer at a time; group commit runs statements in order in one
// session), so a check that reads other rows cannot race with another writer.
private:
 struct FkRT {std::vector<std::vector<std::string>> cols,ref;sql::Expr child_match,parent_match;};
 struct UqRT {std::vector<std::vector<std::string>> cols;sql::Expr match;};
 struct Rules {sql::Constraints c;std::vector<sql::Expr> checks;std::vector<FkRT> fks;std::vector<UqRT> unique;};
 std::map<std::string,Rules> rules_;  // tables with constraints (most have none)
 static std::string quote_ident(const std::string&n){std::string q="\"";for(char c:n){if(c=='"')q+='"';q+=c;}return q+'"';}
 // "a" = ? AND "b" = ? (looks rows up through an index when one exists).
 static sql::Expr match_expr(const std::vector<std::string>&cols){
  std::string t;for(size_t i=0;i<cols.size();i++){if(i)t+=" AND ";t+=quote_ident(cols[i])+" = ?";}
  return sql::parse_expression(t);
 }
 static Rules compile_rules(const std::string&table,sql::Constraints c){
  Rules r;
  for(const auto&k:c.checks){auto e=sql::parse_expression(k.text);sql::strip_qualifier(e,table,table);r.checks.push_back(std::move(e));}
  for(const auto&f:c.fks){
   FkRT x;for(const auto&n:f.cols)x.cols.push_back(json::split_path(n));for(const auto&n:f.ref)x.ref.push_back(json::split_path(n));
   x.child_match=match_expr(f.cols);x.parent_match=match_expr(f.ref);r.fks.push_back(std::move(x));
  }
  for(const auto&u:c.unique){UqRT x;for(const auto&n:u.cols)x.cols.push_back(json::split_path(n));x.match=match_expr(u.cols);r.unique.push_back(std::move(x));}
  r.c=std::move(c);return r;
 }
 static std::string names_json(const std::vector<std::string>&v){std::string s="[";for(size_t i=0;i<v.size();i++){if(i)s+=',';s+=json::quote(v[i]);}return s+"]";}
 static std::string rules_json(const sql::Constraints&c){
  std::string s="{\"checks\":[";
  for(size_t i=0;i<c.checks.size();i++){if(i)s+=',';s+="{\"name\":"+json::quote(c.checks[i].name)+",\"text\":"+json::quote(c.checks[i].text)+"}";}
  s+="],\"foreignKeys\":[";
  for(size_t i=0;i<c.fks.size();i++){
   const auto&f=c.fks[i];if(i)s+=',';
   s+="{\"name\":"+json::quote(f.name)+",\"columns\":"+names_json(f.cols)+",\"table\":"+json::quote(f.table)+",\"references\":"+names_json(f.ref)+",\"onDelete\":"+json::quote(sql::action_name(f.on_delete))+",\"onUpdate\":"+json::quote(sql::action_name(f.on_update))+"}";
  }
  s+="],\"unique\":[";
  for(size_t i=0;i<c.unique.size();i++){if(i)s+=',';s+="{\"name\":"+json::quote(c.unique[i].name)+",\"columns\":"+names_json(c.unique[i].cols)+"}";}
  return s+"]}";
 }
 static sql::Constraints rules_of(const json::Json&j){
  sql::Constraints c;
  auto str=[](const json::Json*v){require(v&&v->is_string(),"CORRUPT catalog constraint");return v->str;};
  auto names=[](const json::Json*v){std::vector<std::string> out;require(v&&v->kind==json::Json::Array,"CORRUPT catalog constraint");for(const auto&x:v->items){require(x.is_string(),"CORRUPT catalog constraint");out.push_back(x.str);}return out;};
  if(const auto*l=j.get("checks"))for(const auto&x:l->items)c.checks.push_back({str(x.get("name")),str(x.get("text"))});
  if(const auto*l=j.get("foreignKeys"))for(const auto&x:l->items){
   sql::ForeignKey f;f.name=str(x.get("name"));f.cols=names(x.get("columns"));f.table=str(x.get("table"));f.ref=names(x.get("references"));
   f.on_delete=sql::action_of(str(x.get("onDelete")));f.on_update=sql::action_of(str(x.get("onUpdate")));c.fks.push_back(std::move(f));
  }
  if(const auto*l=j.get("unique"))for(const auto&x:l->items)c.unique.push_back({str(x.get("name")),names(x.get("columns"))});
  return c;
 }
 std::string all_rules_json()const{
  std::string s;bool first=true;
  for(const auto&[t,r]:rules_){if(r.c.empty())continue;if(!first)s+=',';first=false;s+=json::quote(t)+":"+rules_json(r.c);}
  return s;
 }
 const Rules*rules_for(const std::string&t)const{auto it=rules_.find(t);return it==rules_.end()?nullptr:&it->second;}
 bool referenced(const std::string&t)const{for(const auto&[ct,r]:rules_)for(const auto&f:r.c.fks)if(f.table==t)return true;return false;}
 // Replaces table's constraints and writes the catalog (restored if that fails).
 void publish_rules_locked(const std::string&table,sql::Constraints c){
  std::optional<Rules> prev;if(auto it=rules_.find(table);it!=rules_.end())prev=it->second;
  {std::unique_lock<ReadWriteMutex>r(readers_);if(c.empty())rules_.erase(table);else rules_[table]=compile_rules(table,std::move(c));}
  try{write_catalog_locked(nullptr,nullptr);}
  catch(...){std::unique_lock<ReadWriteMutex>r(readers_);if(prev)rules_[table]=std::move(*prev);else rules_.erase(table);throw;}
 }
 static std::string join_names(const std::vector<std::string>&v){std::string s;for(size_t i=0;i<v.size();i++){if(i)s+='_';s+=v[i];}return s;}
 // Gives new constraints of table names and defaults and checks their targets. have: the
 // table's current constraints; unique_cols: its single-column UNIQUE columns (current and new).
 void prepare_rules_locked(const std::string&table,sql::Constraints&add,const sql::Constraints&have,const std::vector<std::string>&unique_cols){
  std::set<std::string> used;
  for(const auto&x:have.checks)used.insert(x.name);for(const auto&x:have.fks)used.insert(x.name);for(const auto&x:have.unique)used.insert(x.name);
  for(const auto&ix:sec_)if(ix->collection==table)used.insert(ix->name);
  auto claim=[&](std::string&n,const std::string&base){
   if(!n.empty()){if(!used.insert(n).second)throw std::runtime_error("INPUT SQL: constraint name "+n+" is already used in "+table);return;}
   n=base;for(int k=2;!used.insert(n).second;k++)n=base+std::to_string(k);
  };
  for(auto&k:add.checks)claim(k.name,table+"_check");
  for(auto&u:add.unique)claim(u.name,table+"_"+join_names(u.cols)+"_key");
  for(auto&f:add.fks){
   claim(f.name,table+"_"+join_names(f.cols)+"_fkey");
   if(f.ref.empty())f.ref={"id"};
   if(f.ref.size()!=f.cols.size())throw std::runtime_error("INPUT SQL: foreign key "+f.name+" has "+std::to_string(f.cols.size())+" columns but references "+std::to_string(f.ref.size()));
   for(const auto&c:f.cols)if(c=="id")throw std::runtime_error("INPUT SQL: the id column cannot be a foreign key");
   bool self=f.table==table;
   if(!self&&!table_exists(f.table))throw std::runtime_error("INPUT SQL: foreign key "+f.name+" references a missing table: "+f.table);
   // The target must identify one row: the id, a UNIQUE column, or a UNIQUE column set.
   bool ok=f.ref.size()==1&&f.ref[0]=="id";
   if(!ok&&f.ref.size()==1){
    for(const auto&ix:sec_)if(ix->collection==f.table&&ix->unique&&ix->field==f.ref[0])ok=true;
    if(self)for(const auto&c:unique_cols)if(c==f.ref[0])ok=true;
   }
   if(!ok&&f.ref.size()>1){
    std::set<std::string> want(f.ref.begin(),f.ref.end());
    auto match=[&](const std::vector<sql::UniqueDef>&v){for(const auto&u:v)if(std::set<std::string>(u.cols.begin(),u.cols.end())==want)ok=true;};
    if(const auto*r=rules_for(f.table))match(r->c.unique);
    if(self){match(have.unique);match(add.unique);}
    for(const auto&ix:sec_)if(ix->collection==f.table&&ix->unique&&std::set<std::string>(ix->cols.begin(),ix->cols.end())==want)ok=true;  // a composite UNIQUE index
   }
   if(!ok)throw std::runtime_error("INPUT SQL: foreign key "+f.name+" must reference the id, a UNIQUE column or UNIQUE columns of "+f.table);
   if(f.on_delete==sql::ForeignKey::SetNull||f.on_update==sql::ForeignKey::SetNull)
    if(const auto*schema=schema_of(table))for(const auto&c:f.cols)if(const auto*d=column_def(schema,c);d&&d->not_null)
     throw std::runtime_error("INPUT SQL: foreign key "+f.name+" sets "+c+" to NULL, but it is NOT NULL");
  }
 }
 // Indexes that keep constraint checks cheap: on the columns of every foreign key (deleting
 // a parent finds its children) and of every multi-column UNIQUE. Several columns get one
 // composite index, unless an index already starts with them (in any order).
 void rule_indexes_locked(const std::string&table,const sql::Constraints&c){
  auto want=[&](const std::vector<std::string>&cols){
   if(cols.size()==1){for(const auto&ix:sec_)if(ix->collection==table&&ix->field==cols[0]&&!ix->fulltext)return;create_index_locked(table,cols[0],false,"");return;}
   std::set<std::string> set(cols.begin(),cols.end());
   for(const auto&ix:sec_)if(ix->collection==table&&!ix->fulltext&&ix->cols.size()>=cols.size()&&std::set<std::string>(ix->cols.begin(),ix->cols.begin()+std::ptrdiff_t(cols.size()))==set)return;
   if(cols.size()>8||std::count(cols.begin(),cols.end(),"id")){for(const auto&ix:sec_)if(ix->collection==table&&ix->field==cols[0]&&!ix->fulltext)return;create_index_locked(table,cols[0],false,"");return;}  // no composite for these
   create_composite_locked(table,cols,false,"");
  };
  for(const auto&f:c.fks)want(f.cols);
  for(const auto&u:c.unique)want(u.cols);
 }
 static std::optional<std::string> tuple_of(std::string_view doc,const std::vector<std::vector<std::string>>&paths){
  std::string k;for(const auto&p:paths){auto v=json::field(doc,p);if(sql::is_null(v))return std::nullopt;json::encode_into(k,v);}return k;
 }
 static std::vector<json::Scalar> values_of(std::string_view doc,const std::vector<std::vector<std::string>>&paths){
  std::vector<json::Scalar> out;for(const auto&p:paths)out.push_back(json::field(doc,p));return out;
 }
 static std::string tuple_text(const std::vector<std::string>&cols,const std::vector<json::Scalar>&v){
  std::string s="(";for(size_t i=0;i<cols.size();i++){if(i)s+=", ";s+=cols[i];}s+=") = (";
  for(size_t i=0;i<v.size();i++){if(i)s+=", ";s+=sql::to_json(v[i]);}return s+")";
 }
 static bool usable_id(const json::Scalar&v){return (v.tag==json::NUMBER&&v.num==std::floor(v.num)&&std::fabs(v.num)<9e15)||(v.tag==json::STRING&&!v.str.empty());}
 static bool check_fails(const sql::Expr&e,std::string_view doc){
  Params none;sql::Row row{doc,&none};auto v=sql::eval(e,row);return !sql::is_null(v)&&!sql::truthy(v);  // NULL passes, as in SQL
 }
 // Rows of a committed table where cols = vals (the statement-free view). Writer lock held.
 bool committed_row_with(const std::string&table,const std::vector<std::string>&cols,const sql::Expr&match,const std::vector<json::Scalar>&vals){
  if(cols.size()==1&&cols[0]=="id")return usable_id(vals[0])&&cur(row_key(table,vals[0])).has_value();
  bool found=false;scan_rows(table,match,vals,[&](std::string_view,uint64_t,std::string_view){found=true;});return found;
 }
 // New constraints against a table's existing rows (ADD CONSTRAINT, ADD COLUMN). change:
 // how each row will look (ADD COLUMN's default), or null.
 void check_existing_rows_locked(const std::string&table,const Rules&r,const std::function<std::string(std::string_view)>&change){
  if(r.c.empty())return;
  std::vector<std::set<std::string>> fk_tuples(r.fks.size());std::vector<std::vector<std::vector<json::Scalar>>> fk_vals(r.fks.size());
  std::vector<std::unordered_set<std::string>> seen(r.unique.size());
  std::string err;
  scan_rows(table,std::nullopt,Params{},[&](std::string_view,uint64_t,std::string_view doc0){
   if(!err.empty())return;
   std::string changed;std::string_view doc=doc0;if(change){changed=change(doc0);doc=changed;}
   for(size_t k=0;k<r.checks.size();k++)if(check_fails(r.checks[k],doc)){err="CONFLICT SQL: existing rows of "+table+" fail CHECK ("+r.c.checks[k].text+")";return;}
   for(size_t k=0;k<r.unique.size();k++)if(auto t=tuple_of(doc,r.unique[k].cols);t&&!seen[k].insert(*t).second){err="CONFLICT SQL: existing rows of "+table+" repeat "+tuple_text(r.c.unique[k].cols,values_of(doc,r.unique[k].cols));return;}
   for(size_t k=0;k<r.fks.size();k++)if(auto t=tuple_of(doc,r.fks[k].cols);t&&fk_tuples[k].insert(*t).second)fk_vals[k].push_back(values_of(doc,r.fks[k].cols));
  });
  if(!err.empty())throw std::runtime_error(err);
  for(size_t k=0;k<r.fks.size();k++){
   const auto&f=r.c.fks[k];
   for(const auto&v:fk_vals[k])if(!committed_row_with(f.table,f.ref,r.fks[k].parent_match,v))
    throw std::runtime_error("CONFLICT SQL: existing rows of "+table+" break foreign key "+f.name+": no row of "+f.table+" with "+tuple_text(f.ref,v));
  }
 }
 // The constraints, applied to a statement's writes before they are kept or committed. Adds
 // the writes of ON DELETE / ON UPDATE actions to ops. Writer lock held.
 void enforce_rules(std::vector<Op>&ops){
  if(rules_.empty()||ops.empty())return;
  // Tables involved, prepared (indexes built) before any lookup.
  std::set<std::string> involved;
  for(const auto&[t,r]:rules_){involved.insert(t);for(const auto&f:r.c.fks)involved.insert(f.table);}
  bool any=false;for(const auto&o:ops)if(involved.count(std::string(collection_of(o.key)))){any=true;break;}
  if(!any)return;
  for(const auto&t:involved)if(table_exists(t))prepare_locked(t);
  std::unordered_map<std::string,size_t> pos;for(size_t i=0;i<ops.size();i++)pos[ops[i].key]=i;
  // A row as the statement leaves it.
  auto now=[&](const std::string&key)->std::optional<std::string>{
   auto it=pos.find(key);if(it!=pos.end()){const auto&o=ops[it->second];if(o.erase)return std::nullopt;return o.value;}
   return cur(key);
  };
  // Whether a row exists as the statement leaves it (the key index only; no value read).
  auto exists_now=[&](const std::string&key)->bool{
   auto it=pos.find(key);if(it!=pos.end())return !ops[it->second].erase;
   if(auto*ses=active_session()){auto o=ses->overlay.find(key);if(o!=ses->overlay.end())return o->second.has_value();}
   Ref r;auto h=index_hash(key_,key);return lookup(shard_of(h),key,h,r);
  };
  std::unordered_map<std::string,std::optional<std::string>> before;  // as the statement found it
  auto old_of=[&](const std::string&k)->const std::optional<std::string>&{auto it=before.find(k);if(it==before.end())it=before.emplace(k,cur(k)).first;return it->second;};
  // The statement's own rows by column values: (table, columns) -> tuple -> op (stale
  // entries are skipped when used).
  struct Pending {std::string table;std::vector<std::vector<std::string>> paths;std::unordered_multimap<std::string,size_t> m;};
  std::map<std::string,Pending> pending;
  auto note=[&](size_t i){
   if(ops[i].erase)return;auto t=collection_of(ops[i].key);
   for(auto&[sig,p]:pending)if(p.table==t)if(auto k=tuple_of(ops[i].value,p.paths))p.m.emplace(*k,i);
  };
  auto pending_for=[&](const std::string&table,const std::vector<std::string>&cols,const std::vector<std::vector<std::string>>&paths)->Pending&{
   std::string sig=table;for(const auto&c:cols){sig+='\x1f';sig+=c;}
   auto it=pending.find(sig);
   if(it==pending.end()){
    Pending p{table,paths,{}};
    for(size_t i=0;i<ops.size();i++)if(!ops[i].erase&&collection_of(ops[i].key)==table)if(auto k=tuple_of(ops[i].value,paths))p.m.emplace(*k,i);
    it=pending.emplace(sig,std::move(p)).first;
   }
   return it->second;
  };
  // Every row of table with cols = vals as the statement leaves it: cb(key, doc), false stops.
  auto rows_with=[&](const std::string&table,const std::vector<std::string>&cols,const std::vector<std::vector<std::string>>&paths,const sql::Expr&match,const std::vector<json::Scalar>&vals,const std::function<bool(const std::string&,const std::string&)>&cb){
   if(cols.size()==1&&cols[0]=="id"){
    if(!usable_id(vals[0]))return;auto key=row_key(table,vals[0]);if(auto d=now(key))cb(key,*d);return;
   }
   std::string want;for(const auto&v:vals)json::encode_into(want,v);
   std::vector<std::pair<std::string,std::string>> found;std::string prefix=table+'\0';
   scan_rows(table,match,vals,[&](std::string_view user,uint64_t,std::string_view doc){
    std::string key=prefix+std::string(user);if(!pos.count(key))found.emplace_back(std::move(key),std::string(doc));
   });
   std::unordered_set<std::string> done;
   auto&p=pending_for(table,cols,paths);auto range=p.m.equal_range(want);
   for(auto it=range.first;it!=range.second;++it){
    const auto&o=ops[it->second];if(o.erase||pos[o.key]!=it->second)continue;
    auto k=tuple_of(o.value,paths);if(!k||*k!=want||!done.insert(o.key).second)continue;
    found.emplace_back(o.key,o.value);
   }
   for(const auto&[k,d]:found)if(!cb(k,d))return;
  };
  // Sets (or adds) the write of key; it is checked again.
  std::deque<size_t> work;for(size_t i=0;i<ops.size();i++)work.push_back(i);
  auto put=[&](const std::string&key,std::string value,bool erase){
   auto it=pos.find(key);size_t i;
   if(it!=pos.end()){i=it->second;ops[i].value=std::move(value);ops[i].erase=erase;ops[i].expected=ANY;}
   else{i=ops.size();ops.push_back({key,std::move(value),ANY,erase});pos[key]=i;}
   require(ops.size()<=65536,"LIMIT a statement can change at most 65,536 rows (foreign key actions included)");
   note(i);work.push_back(i);
  };
  size_t steps=0;
  while(!work.empty()){
   size_t i=work.front();work.pop_front();
   require(++steps<=1000000,"LIMIT foreign key actions do not finish (a cycle of ON UPDATE CASCADE?)");
   const std::string key=ops[i].key;
   if(pos[key]!=i)continue;
   std::string table(collection_of(key));
   if(!involved.count(table)||!table_exists(table))continue;
   const Rules*R=rules_for(table);
   std::optional<std::string> nv;if(!ops[i].erase)nv=ops[i].value;
   const auto&old=old_of(key);
   if(R&&nv){
    for(size_t k=0;k<R->checks.size();k++)if(check_fails(R->checks[k],*nv))
     throw std::runtime_error("CONFLICT SQL: CHECK ("+R->c.checks[k].text+") failed for a row of "+table+" (constraint "+R->c.checks[k].name+")");
    for(size_t k=0;k<R->unique.size();k++){
     const auto&u=R->unique[k];auto t=tuple_of(*nv,u.cols);if(!t)continue;
     if(old&&tuple_of(*old,u.cols)==t)continue;
     auto vals=values_of(*nv,u.cols);
     rows_with(table,R->c.unique[k].cols,u.cols,u.match,vals,[&](const std::string&k2,const std::string&){
      if(k2==key)return true;
      throw std::runtime_error("CONFLICT SQL: duplicate "+tuple_text(R->c.unique[k].cols,vals)+" in "+table+" (UNIQUE constraint "+R->c.unique[k].name+")");
     });
    }
    for(size_t k=0;k<R->fks.size();k++){
     const auto&f=R->c.fks[k];const auto&x=R->fks[k];auto t=tuple_of(*nv,x.cols);if(!t)continue;
     if(old&&tuple_of(*old,x.cols)==t)continue;  // unchanged (a deleted parent is checked from its side)
     auto vals=values_of(*nv,x.cols);bool found=false;
     if(f.ref.size()==1&&f.ref[0]=="id")found=usable_id(vals[0])&&exists_now(row_key(f.table,vals[0]));
     else rows_with(f.table,f.ref,x.ref,x.parent_match,vals,[&](const std::string&,const std::string&){found=true;return false;});
     if(!found)throw std::runtime_error("CONFLICT SQL: foreign key "+f.name+" failed: no row of "+f.table+" with "+tuple_text(f.ref,vals));
    }
   }
   // Rows of other tables (or this one) that reference the old values.
   if(!old)continue;
   for(const auto&[ct,CR]:rules_)for(size_t k=0;k<CR.fks.size();k++){
    const auto&f=CR.c.fks[k];if(f.table!=table)continue;
    const auto&x=CR.fks[k];auto ot=tuple_of(*old,x.ref);if(!ot)continue;
    if(nv&&tuple_of(*nv,x.ref)==ot)continue;
    auto ovals=values_of(*old,x.ref);
    std::vector<std::pair<std::string,std::string>> kids;
    rows_with(ct,f.cols,x.cols,x.child_match,ovals,[&](const std::string&ck,const std::string&cd){kids.emplace_back(ck,cd);return true;});
    if(kids.empty())continue;
    auto action=nv?f.on_update:f.on_delete;
    if(action==sql::ForeignKey::NoAction||action==sql::ForeignKey::Restrict)
     throw std::runtime_error("CONFLICT SQL: foreign key "+f.name+": rows of "+ct+" still reference "+table+" "+tuple_text(f.ref,ovals)+(nv?" (ON UPDATE ":" (ON DELETE ")+sql::action_name(action)+")");
    const auto*schema=schema_of(ct);
    for(auto&[ck,cd]:kids){
     if(action==sql::ForeignKey::Cascade&&!nv){put(ck,"",true);continue;}
     auto d=json::parse(cd);
     for(size_t c=0;c<f.cols.size();c++){
      json::Scalar v;v.tag=json::NUL;
      if(action==sql::ForeignKey::Cascade)v=json::field(*nv,x.ref[c]);
      else if(action==sql::ForeignKey::SetDefault){if(const auto*def=column_def(schema,f.cols[c]);def&&def->has_default)v=def->def;}
      if(const auto*def=column_def(schema,f.cols[c]))check_value(ct,*def,v);
      json::set_path(d,x.cols[c],json_of(v));
     }
     put(ck,json::dump(d),false);
    }
   }
  }
 }
public:
