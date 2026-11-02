// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'nxs_search_result.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

NexusModsSearchResult _$NexusModsSearchResultFromJson(
  Map<String, dynamic> json,
) => NexusModsSearchResult(
  terms: (json['terms'] as List<dynamic>).map((e) => e as String).toList(),
  excludeAuthors: json['exclude_authors'] as List<dynamic>,
  excludeTags: json['exclude_tags'] as List<dynamic>,
  includeAdult: json['include_adult'] as bool,
  took: (json['took'] as num).toInt(),
  total: (json['total'] as num).toInt(),
  results: (json['results'] as List<dynamic>)
      .map((e) => Result.fromJson(e as Map<String, dynamic>))
      .toList(),
);

Map<String, dynamic> _$NexusModsSearchResultToJson(
  NexusModsSearchResult instance,
) => <String, dynamic>{
  'terms': instance.terms,
  'exclude_authors': instance.excludeAuthors,
  'exclude_tags': instance.excludeTags,
  'include_adult': instance.includeAdult,
  'took': instance.took,
  'total': instance.total,
  'results': instance.results,
};

Result _$ResultFromJson(Map<String, dynamic> json) => Result(
  name: json['name'] as String,
  downloads: (json['downloads'] as num).toInt(),
  endorsements: (json['endorsements'] as num).toInt(),
  url: json['url'] as String,
  image: json['image'] as String,
  username: json['username'] as String,
  userId: (json['user_id'] as num).toInt(),
  gameName: $enumDecode(_$GameNameEnumMap, json['game_name']),
  gameId: (json['game_id'] as num).toInt(),
  modId: (json['mod_id'] as num).toInt(),
  adult: json['adult'] as bool,
);

Map<String, dynamic> _$ResultToJson(Result instance) => <String, dynamic>{
  'name': instance.name,
  'downloads': instance.downloads,
  'endorsements': instance.endorsements,
  'url': instance.url,
  'image': instance.image,
  'username': instance.username,
  'user_id': instance.userId,
  'game_name': _$GameNameEnumMap[instance.gameName]!,
  'game_id': instance.gameId,
  'mod_id': instance.modId,
  'adult': instance.adult,
};

const _$GameNameEnumMap = {
  GameName.STARWARSBATTLEFRONT22017: 'starwarsbattlefront22017',
};
