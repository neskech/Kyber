// GENERATED CODE - DO NOT MODIFY BY HAND
// coverage:ignore-file
// ignore_for_file: type=lint, type=warning, deprecated_member_use, deprecated_member_use_from_same_package
// ignore_for_file: unused_element, deprecated_member_use, deprecated_member_use_from_same_package, use_function_type_syntax_for_parameters, unnecessary_const, avoid_init_to_null, invalid_override_different_default_values_named, prefer_expression_function_bodies, annotate_overrides, invalid_annotation_target, unnecessary_question_mark

part of 'frosty_collection_manifest.dart';

// **************************************************************************
// FreezedGenerator
// **************************************************************************

// GENERATED CODE - DO NOT MODIFY BY HAND
// dart format off
T _$identity<T>(T value) => value;

/// @nodoc
mixin _$FrostyCollectionManifest {

 String get link; String get title; String get author; String get version; String get description; String get category; List<String> get mods; List<String> get modVersions;
/// Create a copy of FrostyCollectionManifest
/// with the given fields replaced by the non-null parameter values.
@JsonKey(includeFromJson: false, includeToJson: false)
@pragma('vm:prefer-inline')
$FrostyCollectionManifestCopyWith<FrostyCollectionManifest> get copyWith => _$FrostyCollectionManifestCopyWithImpl<FrostyCollectionManifest>(this as FrostyCollectionManifest, _$identity);

  /// Serializes this FrostyCollectionManifest to a JSON map.
  Map<String, dynamic> toJson();


@override
bool operator ==(Object other) {
  final _this = this as FrostyCollectionManifest;
  return identical(this, other) || (other.runtimeType == runtimeType&&other is FrostyCollectionManifest&&(identical(other.link, _this.link) || other.link == _this.link)&&(identical(other.title, _this.title) || other.title == _this.title)&&(identical(other.author, _this.author) || other.author == _this.author)&&(identical(other.version, _this.version) || other.version == _this.version)&&(identical(other.description, _this.description) || other.description == _this.description)&&(identical(other.category, _this.category) || other.category == _this.category)&&const DeepCollectionEquality().equals(other.mods, _this.mods)&&const DeepCollectionEquality().equals(other.modVersions, _this.modVersions));
}

@JsonKey(includeFromJson: false, includeToJson: false)
@override
int get hashCode {
  final _this = this as FrostyCollectionManifest;
  return Object.hash(runtimeType,_this.link,_this.title,_this.author,_this.version,_this.description,_this.category,const DeepCollectionEquality().hash(_this.mods),const DeepCollectionEquality().hash(_this.modVersions));
}

@override
String toString() {
  final _this = this as FrostyCollectionManifest;
  return 'FrostyCollectionManifest(link: ${_this.link}, title: ${_this.title}, author: ${_this.author}, version: ${_this.version}, description: ${_this.description}, category: ${_this.category}, mods: ${_this.mods}, modVersions: ${_this.modVersions})';
}


}

/// @nodoc
abstract mixin class $FrostyCollectionManifestCopyWith<$Res>  {
  factory $FrostyCollectionManifestCopyWith(FrostyCollectionManifest value, $Res Function(FrostyCollectionManifest) _then) = _$FrostyCollectionManifestCopyWithImpl;
@useResult
$Res call({
 String link, String title, String author, String version, String description, String category, List<String> mods, List<String> modVersions
});




}
/// @nodoc
class _$FrostyCollectionManifestCopyWithImpl<$Res>
    implements $FrostyCollectionManifestCopyWith<$Res> {
  _$FrostyCollectionManifestCopyWithImpl(this._self, this._then);

  final FrostyCollectionManifest _self;
  final $Res Function(FrostyCollectionManifest) _then;

/// Create a copy of FrostyCollectionManifest
/// with the given fields replaced by the non-null parameter values.
@pragma('vm:prefer-inline') @override $Res call({Object? link = null,Object? title = null,Object? author = null,Object? version = null,Object? description = null,Object? category = null,Object? mods = null,Object? modVersions = null,}) {
  return _then(FrostyCollectionManifest(
link: null == link ? _self.link : link // ignore: cast_nullable_to_non_nullable
as String,title: null == title ? _self.title : title // ignore: cast_nullable_to_non_nullable
as String,author: null == author ? _self.author : author // ignore: cast_nullable_to_non_nullable
as String,version: null == version ? _self.version : version // ignore: cast_nullable_to_non_nullable
as String,description: null == description ? _self.description : description // ignore: cast_nullable_to_non_nullable
as String,category: null == category ? _self.category : category // ignore: cast_nullable_to_non_nullable
as String,mods: null == mods ? _self.mods : mods // ignore: cast_nullable_to_non_nullable
as List<String>,modVersions: null == modVersions ? _self.modVersions : modVersions // ignore: cast_nullable_to_non_nullable
as List<String>,
  ));
}

}


/// Adds pattern-matching-related methods to [FrostyCollectionManifest].
extension FrostyCollectionManifestPatterns on FrostyCollectionManifest {
/// A variant of `map` that fallback to returning `orElse`.
///
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case final Subclass value:
///     return ...;
///   case _:
///     return orElse();
/// }
/// ```

@optionalTypeArgs TResult maybeMap<TResult extends Object?>(TResult Function( _FrostyCollectionManifest value)?  $default,{required TResult orElse(),}){
final _that = this;
switch (_that) {
case _FrostyCollectionManifest() when $default != null:
return $default(_that);case _:
  return orElse();

}
}
/// A `switch`-like method, using callbacks.
///
/// Callbacks receives the raw object, upcasted.
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case final Subclass value:
///     return ...;
///   case final Subclass2 value:
///     return ...;
/// }
/// ```

@optionalTypeArgs TResult map<TResult extends Object?>(TResult Function( _FrostyCollectionManifest value)  $default,){
final _that = this;
switch (_that) {
case _FrostyCollectionManifest():
return $default(_that);case _:
  throw StateError('Unexpected subclass');

}
}
/// A variant of `map` that fallback to returning `null`.
///
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case final Subclass value:
///     return ...;
///   case _:
///     return null;
/// }
/// ```

@optionalTypeArgs TResult? mapOrNull<TResult extends Object?>(TResult? Function( _FrostyCollectionManifest value)?  $default,){
final _that = this;
switch (_that) {
case _FrostyCollectionManifest() when $default != null:
return $default(_that);case _:
  return null;

}
}
/// A variant of `when` that fallback to an `orElse` callback.
///
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case Subclass(:final field):
///     return ...;
///   case _:
///     return orElse();
/// }
/// ```

@optionalTypeArgs TResult maybeWhen<TResult extends Object?>(TResult Function( String link,  String title,  String author,  String version,  String description,  String category,  List<String> mods,  List<String> modVersions)?  $default,{required TResult orElse(),}) {final _that = this;
switch (_that) {
case _FrostyCollectionManifest() when $default != null:
return $default(_that.link,_that.title,_that.author,_that.version,_that.description,_that.category,_that.mods,_that.modVersions);case _:
  return orElse();

}
}
/// A `switch`-like method, using callbacks.
///
/// As opposed to `map`, this offers destructuring.
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case Subclass(:final field):
///     return ...;
///   case Subclass2(:final field2):
///     return ...;
/// }
/// ```

@optionalTypeArgs TResult when<TResult extends Object?>(TResult Function( String link,  String title,  String author,  String version,  String description,  String category,  List<String> mods,  List<String> modVersions)  $default,) {final _that = this;
switch (_that) {
case _FrostyCollectionManifest():
return $default(_that.link,_that.title,_that.author,_that.version,_that.description,_that.category,_that.mods,_that.modVersions);case _:
  throw StateError('Unexpected subclass');

}
}
/// A variant of `when` that fallback to returning `null`
///
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case Subclass(:final field):
///     return ...;
///   case _:
///     return null;
/// }
/// ```

@optionalTypeArgs TResult? whenOrNull<TResult extends Object?>(TResult? Function( String link,  String title,  String author,  String version,  String description,  String category,  List<String> mods,  List<String> modVersions)?  $default,) {final _that = this;
switch (_that) {
case _FrostyCollectionManifest() when $default != null:
return $default(_that.link,_that.title,_that.author,_that.version,_that.description,_that.category,_that.mods,_that.modVersions);case _:
  return null;

}
}

}

/// @nodoc
@JsonSerializable()

class _FrostyCollectionManifest implements FrostyCollectionManifest {
  const _FrostyCollectionManifest({required this.link, required this.title, required this.author, required this.version, required this.description, required this.category, required  List<String> mods, required  List<String> modVersions}): _mods = mods,_modVersions = modVersions;
  factory _FrostyCollectionManifest.fromJson(Map<String, dynamic> json) => _$FrostyCollectionManifestFromJson(json);

@override final  String link;
@override final  String title;
@override final  String author;
@override final  String version;
@override final  String description;
@override final  String category;
 final  List<String> _mods;
@override List<String> get mods {
  if (_mods is EqualUnmodifiableListView) return _mods;
  // ignore: implicit_dynamic_type
  return EqualUnmodifiableListView(_mods);
}

 final  List<String> _modVersions;
@override List<String> get modVersions {
  if (_modVersions is EqualUnmodifiableListView) return _modVersions;
  // ignore: implicit_dynamic_type
  return EqualUnmodifiableListView(_modVersions);
}


/// Create a copy of FrostyCollectionManifest
/// with the given fields replaced by the non-null parameter values.
@override @JsonKey(includeFromJson: false, includeToJson: false)
@pragma('vm:prefer-inline')
_$FrostyCollectionManifestCopyWith<_FrostyCollectionManifest> get copyWith => __$FrostyCollectionManifestCopyWithImpl<_FrostyCollectionManifest>(this, _$identity);

@override
Map<String, dynamic> toJson() {
  return _$FrostyCollectionManifestToJson(this, );
}

@override
bool operator ==(Object other) {
    return identical(this, other) || (other.runtimeType == runtimeType&&other is _FrostyCollectionManifest&&(identical(other.link, link) || other.link == link)&&(identical(other.title, title) || other.title == title)&&(identical(other.author, author) || other.author == author)&&(identical(other.version, version) || other.version == version)&&(identical(other.description, description) || other.description == description)&&(identical(other.category, category) || other.category == category)&&const DeepCollectionEquality().equals(other.mods, _mods)&&const DeepCollectionEquality().equals(other.modVersions, _modVersions));
}

@JsonKey(includeFromJson: false, includeToJson: false)
@override
int get hashCode {
    return Object.hash(runtimeType,link,title,author,version,description,category,const DeepCollectionEquality().hash(_mods),const DeepCollectionEquality().hash(_modVersions));
}

@override
String toString() {
    return 'FrostyCollectionManifest(link: $link, title: $title, author: $author, version: $version, description: $description, category: $category, mods: $mods, modVersions: $modVersions)';
}


}

/// @nodoc
abstract mixin class _$FrostyCollectionManifestCopyWith<$Res> implements $FrostyCollectionManifestCopyWith<$Res> {
  factory _$FrostyCollectionManifestCopyWith(_FrostyCollectionManifest value, $Res Function(_FrostyCollectionManifest) _then) = __$FrostyCollectionManifestCopyWithImpl;
@override @useResult
$Res call({
 String link, String title, String author, String version, String description, String category, List<String> mods, List<String> modVersions
});




}
/// @nodoc
class __$FrostyCollectionManifestCopyWithImpl<$Res>
    implements _$FrostyCollectionManifestCopyWith<$Res> {
  __$FrostyCollectionManifestCopyWithImpl(this._self, this._then);

  final _FrostyCollectionManifest _self;
  final $Res Function(_FrostyCollectionManifest) _then;

/// Create a copy of FrostyCollectionManifest
/// with the given fields replaced by the non-null parameter values.
@override @pragma('vm:prefer-inline') $Res call({Object? link = null,Object? title = null,Object? author = null,Object? version = null,Object? description = null,Object? category = null,Object? mods = null,Object? modVersions = null,}) {
  return _then(_FrostyCollectionManifest(
link: null == link ? _self.link : link // ignore: cast_nullable_to_non_nullable
as String,title: null == title ? _self.title : title // ignore: cast_nullable_to_non_nullable
as String,author: null == author ? _self.author : author // ignore: cast_nullable_to_non_nullable
as String,version: null == version ? _self.version : version // ignore: cast_nullable_to_non_nullable
as String,description: null == description ? _self.description : description // ignore: cast_nullable_to_non_nullable
as String,category: null == category ? _self.category : category // ignore: cast_nullable_to_non_nullable
as String,mods: null == mods ? _self._mods : mods // ignore: cast_nullable_to_non_nullable
as List<String>,modVersions: null == modVersions ? _self._modVersions : modVersions // ignore: cast_nullable_to_non_nullable
as List<String>,
  ));
}


}

// dart format on
