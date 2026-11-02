// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'mod_collection.dart';

// **************************************************************************
// TypeAdapterGenerator
// **************************************************************************

class ModCollectionMetaDataAdapter extends TypeAdapter<ModCollectionMetaData> {
  @override
  final typeId = 30;

  @override
  ModCollectionMetaData read(BinaryReader reader) {
    final numOfFields = reader.readByte();
    final fields = <int, dynamic>{
      for (int i = 0; i < numOfFields; i++) reader.readByte(): reader.read(),
    };
    return ModCollectionMetaData(
      localId: fields[0] as String,
      title: fields[1] as String,
      description: fields[2] == null ? '' : fields[2] as String?,
      mods: (fields[3] as List).cast<CollectionMod>(),
      isCosmetic: fields[4] == null ? false : fields[4] as bool,
      icon: fields[5] as Uint8List?,
    );
  }

  @override
  void write(BinaryWriter writer, ModCollectionMetaData obj) {
    writer
      ..writeByte(6)
      ..writeByte(0)
      ..write(obj.localId)
      ..writeByte(1)
      ..write(obj.title)
      ..writeByte(2)
      ..write(obj.description)
      ..writeByte(3)
      ..write(obj.mods)
      ..writeByte(4)
      ..write(obj.isCosmetic)
      ..writeByte(5)
      ..write(obj.icon);
  }

  @override
  int get hashCode => typeId.hashCode;

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is ModCollectionMetaDataAdapter &&
          runtimeType == other.runtimeType &&
          typeId == other.typeId;
}

class CollectionModAdapter extends TypeAdapter<CollectionMod> {
  @override
  final typeId = 31;

  @override
  CollectionMod read(BinaryReader reader) {
    final numOfFields = reader.readByte();
    final fields = <int, dynamic>{
      for (int i = 0; i < numOfFields; i++) reader.readByte(): reader.read(),
    };
    return CollectionMod(
      name: fields[0] as String,
      version: fields[1] as String,
      link: fields[2] as String,
      isCollection: fields[3] == null ? false : fields[3] as bool,
      mods: (fields[4] as List?)?.cast<String>(),
      filename: fields[5] as String?,
    );
  }

  @override
  void write(BinaryWriter writer, CollectionMod obj) {
    writer
      ..writeByte(6)
      ..writeByte(0)
      ..write(obj.name)
      ..writeByte(1)
      ..write(obj.version)
      ..writeByte(2)
      ..write(obj.link)
      ..writeByte(3)
      ..write(obj.isCollection)
      ..writeByte(4)
      ..write(obj.mods)
      ..writeByte(5)
      ..write(obj.filename);
  }

  @override
  int get hashCode => typeId.hashCode;

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is CollectionModAdapter &&
          runtimeType == other.runtimeType &&
          typeId == other.typeId;
}
